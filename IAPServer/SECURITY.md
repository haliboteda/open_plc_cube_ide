# IAP Security Design

Authentication/verification design for the OpenPLC firmware-update path:
bootloader (this repo), the running application (Arduino core), and the PC
upload tool. Covers both transports (USB CDC and Ethernet/TCP) and the UDP
discovery protocol.

## End-to-end flow

Each step is labeled by what it actually guarantees: a **self-test** (is the
verification code itself working), an **integrity check** (did the bits
survive transport, no key involved), or an **authentication** (does the
other party hold a secret it should hold).

**0. Crypto self-test** (self-test, once per boot) — `bootloader_state_init()`
runs `sha256_selftest()` against known FIPS 180-4 / RFC 4231 vectors. Confirms
the SHA-256/HMAC implementation in this running binary computes correctly;
does not check the bootloader's or app's integrity. Runs once per power
cycle — nothing changes between operations within one session, so re-running
it per-command would add cost for no new information.

**1. Boot-time signature verification** (self-check) — `server_init()`
re-hashes the actual app flash region and verifies it against the ECDSA
signature stored in the last accepted metadata record (`fw_verify_signature`,
public key in `fw_pubkey.c`). Runs every boot, not just once after flashing,
because flash content can degrade or be tampered with after a successful
write. Failure keeps the device in the bootloader.

**2. Discovery** (intentionally unauthenticated) — `udp_server_recv()`
replies to `DISCOVER`/`openplc_discover`/`openplc_server_where_r_y`/`ping`
with `<deviceName>_<uidHex>_<role>_<version>` (`role` = `BOOTLD` or
`BOOTLD-INVALID` if Step 1 failed). No auth by design: the UID in the reply
is the lookup key a per-device secret would be indexed by, so requiring that
secret just to discover the device would be circular. Nothing that changes
device state depends on this being trustworthy — the trust boundary is
Steps 3-6. Residual risk: an attacker spoofing a source IP on the same
broadcast domain could use this as a small reflection/amplification
primitive; mitigated by rate-limiting, not auth — `discovery_reply_allowed()`
tracks up to 8 recent sources and won't reply to the same one more than once
per 2s (LRU-evicted, not a security boundary, just an abuse cap).

**3. Entering upload mode** — two triggers, held to different standards:
- Network: `openplc_server_reboot_challenge` / `openplc_server_reboot
  <hmac_hex>` requires a valid HMAC-SHA256 over a fresh one-shot, 30s-TTL
  nonce (`iap_auth.c`). Wrong/missing HMAC is logged and ignored.
- USB CDC: the 1200bps "magic touch" (`CDC_SET_LINE_CODING` in
  `usbd_cdc_if.c`) resets straight into the bootloader with no
  authentication possible — it's a one-way baud-rate signal with no return
  channel for challenge-response. Accepted because reaching it already
  requires physical USB access, a higher trust tier than routing a UDP
  packet to an IP. It only arms the waiting-for-upload state; actual
  authorization still happens in Step 4.

Both write the same `MAGIC_BKP_REG` flag and reset. The asymmetry is a
deliberate, accepted tradeoff, not a gap.

**4. Upload session authorization** (authentication) — `process_command()`
(`IAP_server.c`) is one state machine shared verbatim by CDC and Ethernet;
only `send_response()` routes differently. `flash <size> <crc32_hex>
<signature_hex> <hmac_hex> [version]` requires `hmac_hex ==
HMAC-SHA256(shared key, nonce || "flash <size> <crc32hex> <signature_hex>[
<version>]")` for the most recent nonce (constant-time compare, nonce
consumed either way). Proves the caller holds the key authorized to start an
update. Failure (or missing signature) is rejected *before* erasing flash,
so a bad request can't brick a working app for nothing. Identical for both
transports — no CDC-specific work needed here.

The trailing `version` field is optional (an older PC tool that never sends
it still works, saved as `0`) and is *not* used by the bootloader to block
anything — a validly signed older image is always accepted. It exists so a
`getversion` query (same command handler, no auth needed — it only reads
back a non-secret number) lets the PC tool learn the currently-installed
version before it sends `flash`, and warn the operator in its own console if
what it's about to push is older. The decision to proceed through a
downgrade is the operator's, made outside the device.

**5. Data transfer** (integrity only) — CRC32 over the full image once
received (`HAL_CRC_Calculate`). Catches transmission corruption; proves
nothing about who sent it or whether it was deliberately altered (trivial to
recompute after tampering).

**6. Post-transfer signature verification** (authentication) — same
`fw_verify_signature()` call as Step 1, over the freshly written region.
Answers a different question than Step 4: not "is this session authorized"
but "was this exact binary signed by the release key." An authorized session
uploading an unsigned/tampered image is still rejected here.

**7. Commit** — `bootloader_state_save_metadata()` appends the new
(size, version, SHA-256, signature) record; `bootloader_state_log_event(...)`
appends a tamper-chained log entry (each entry's stored hash covers the
previous entry's raw bytes) carrying the event type, transport, `peer_ip`
(TCP only — always `0` for CDC, USB carries no address-equivalent identity
to log), `HAL_GetTick()` at the time of the event, and the current
`iap_auth` challenge counter (ties an `AUTH_FAIL`/`SIG_FAIL` entry to a
specific challenge attempt instead of just "a failure happened at some
point"). Device reboots into Step 0/1, which re-verifies from scratch
rather than trusting this session's own success report.

## Two independent keys

- **ECDSA P-256 keypair** (`IAPServer/keys/`) — decides whether the
  bootloader trusts a firmware image enough to execute it (Steps 1, 6). One
  keypair per release line; private key never touches a device.
- **Per-device HMAC secret** (`iap_auth.c`) — decides who may *start* an
  update session (Steps 3 network trigger, 4).

Separate on purpose: extracting a device's HMAC secret (e.g. physical
compromise) still can't forge a firmware signature, since the ECDSA private
key was never on the device.

## Accepted asymmetries (documented so they aren't mistaken for bugs)

- CDC's 1200bps trigger has no auth — requires physical access, a different
  trust tier than network.
- Discovery replies are unauthenticated by design — see Step 2.
- `iap_auth.c`'s nonce state is a single global — only one challenge in
  flight at a time. Concurrent CDC + Ethernet challenge requests would have
  the second overwrite the first's nonce, failing the first session's
  `flash` auth. Availability quirk, not a security hole (a nonce can never
  be double-consumed or accepted after being overwritten).

## Keys — must be rotated before production

- **ECDSA signing key**: `fw_signing_key.TEST_ONLY.pem` is a public test
  key — anyone with this repo can sign an image the placeholder public key
  accepts. Run `generate_keys.sh`, paste the new public key into
  `fw_pubkey.c`, keep the private key off any repo/network.
- **HMAC session key**: currently one fixed value hardcoded identically in
  three places (this repo's `iap_auth.c`, Arduino core's `iap_auth.c`, PC
  tool's `auth.go`). Should be one independent key per device, provisioned
  at manufacturing time and looked up by UID — that provisioning process
  doesn't exist yet and isn't something this code alone can provide.

## TODO

- [ ] **Flash Option Bytes (WRP) on the bootloader sector.** Nothing today
      stops a Flash write primitive (bug elsewhere, or direct SWD/JTAG) from
      overwriting the bootloader itself or the embedded public key in
      `fw_pubkey.c` — every check above assumes the bootloader performing it
      hasn't been replaced. WRP enforces that at the hardware level. Once
      set, bootloader updates require physical SWD access (ST-Link/J-Link +
      STM32CubeProgrammer) to clear WRP, reflash, and re-lock — document
      this as a factory/service procedure, not part of the end-user Arduino
      IDE flow. If RDP is also considered: check the STM32H7 reference
      manual first — lowering RDP triggers a full chip mass-erase by design,
      wiping bootloader and app and requiring full re-provisioning.
- [ ] **Per-device HMAC provisioning + real release ECDSA key** — the actual
      manufacturing-line process.
- [ ] **PC-tool downgrade prompt.** The bootloader-side foundation is done
      (Step 4: `flash` carries a real version, `getversion` reports what's
      installed, downgrades are never blocked device-side). What's still
      missing is entirely on the PC-tool side: before sending `flash`, query
      `getversion`, compare against the image about to be sent, and if it's
      a downgrade, print a warning and require an explicit yes in that
      tool's own console before proceeding. Needs access to that repo (not
      part of this workspace) to implement.
- [ ] **Consolidate `iap_auth.c`/`sha256.c` across the three repos** into one
      shared source instead of three hand-synced copies.

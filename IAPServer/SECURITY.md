# IAP Security Design

Authentication/verification design for the OpenPLC firmware-update path:
bootloader (this repo), the running application (Arduino core), and the PC
upload tool. Covers both transports (USB CDC and Ethernet/TCP) and the UDP
discovery protocol.

## End-to-end flow

Each step is labeled by what it actually guarantees: a **self-test** (is the
verification code itself working), an **integrity check** (did the bits
survive transport, no key involved), or an **authentication** (does the
other party hold a private key some trusted root authorised).

**0. Crypto self-test** (self-test, once per boot) - `bootloader_state_init()`
runs `sha256_selftest()` against known FIPS 180-4 vectors. Confirms the
SHA-256 implementation in this running binary computes correctly; does not
check the bootloader's or app's integrity. Runs once per power cycle -
nothing changes between operations within one session, so re-running it
per-command would add cost for no new information.

**1. Boot-time signature verification** (self-check) - `server_decide()`
re-hashes the actual app flash region and checks it against the last accepted
metadata record, which holds both the image signature and the certificate that
authorised it (`iap_cert_verify_image`). Two questions, both re-asked every
boot: is that certificate signed by the root this board currently trusts
(`owner_slot_root()`), and did the certificate's leaf key sign this image.
Storing the whole certificate rather than a cached leaf key is what makes a
change of owner retroactively invalidate the installed firmware - the stored
signature only verifies under the root that issued it. Runs every boot, not
just once after flashing, because flash content can degrade or be tampered
with after a successful write. Failure keeps the device in the bootloader.

**2. Discovery** (intentionally unauthenticated) - `udp_server_recv()`
replies to `DISCOVER`/`openplc_discover`/`openplc_server_where_r_y`/`ping`
with `<deviceName>_<uidHex>_<role>_<version>` (`role` = `BOOTLD` or
`BOOTLD-INVALID` if Step 1 failed). No auth by design: the UID in the reply
is the lookup key a per-device secret would be indexed by, so requiring that
secret just to discover the device would be circular. Nothing that changes
device state depends on this being trustworthy - the trust boundary is
Steps 3-6. Residual risk: an attacker spoofing a source IP on the same
broadcast domain could use this as a small reflection/amplification
primitive; mitigated by rate-limiting, not auth - `discovery_reply_allowed()`
tracks up to 8 recent sources and won't reply to the same one more than once
per 2s (LRU-evicted, not a security boundary, just an abuse cap).

**3. Entering upload mode** - two triggers, held to different standards:
- Network: `openplc_server_reboot_challenge` / `openplc_server_reboot
  <cert_hex> <noncesig_hex>` requires a certificate the trusted root vouches
  for, plus an ECDSA signature by that certificate's leaf key over a fresh
  one-shot, 30s-TTL nonce (`iap_auth.c`). A wrong or missing signature is
  logged and ignored. The running application answers this one, and resolves
  the trusted root by reading the bootloader's owner-record area directly
  (`owner_root_ro.c` in the Arduino core) - there is no other channel that
  would hand it that root.
- USB CDC: the 1200bps "magic touch" (`CDC_SET_LINE_CODING` in
  `usbd_cdc_if.c`) resets straight into the bootloader with no
  authentication possible - it's a one-way baud-rate signal with no return
  channel for challenge-response. Accepted because reaching it already
  requires physical USB access, a higher trust tier than routing a UDP
  packet to an IP. It only arms the waiting-for-upload state; actual
  authorization still happens in Step 4.

Both write the same `MAGIC_BKP_REG` flag and reset. The asymmetry is a
deliberate, accepted tradeoff, not a gap.

**4. Upload session authorization** (authentication) - `process_command()`
(`IAP_server.c`) is one state machine shared verbatim by CDC and Ethernet;
only `send_response()` routes differently. `flash <size> <crc32_hex>
<signature_hex> <cert_hex> <noncesig_hex>` requires two things to hold: the
certificate verifies against the root this board trusts, and `noncesig_hex` is
an ECDSA signature by that certificate's leaf key over
`sha256(nonce || "flash <size> <crc32hex> <signature_hex>")` for the most
recent nonce (consumed either way). Proves the caller holds a private key some
trusted root authorised - not that they hold a shared secret, of which the
board has none. Failure (or missing signature) is rejected *before* erasing
flash, so a bad request can't brick a working app for nothing. Identical for
both transports - no CDC-specific work needed here.

**5. Data transfer** (integrity only) - CRC32 over the full image once
received (`HAL_CRC_Calculate`). Catches transmission corruption; proves
nothing about who sent it or whether it was deliberately altered (trivial to
recompute after tampering).

**6. Post-transfer signature verification** (authentication) - same
`iap_cert_verify_image()` call as Step 1, over the image staged in SDRAM
before the application region is touched. Answers a different question than
Step 4: not "is this session authorized" but "was this exact binary signed by
the key that certificate names." An authorized session uploading an
unsigned/tampered image is still rejected here.

**7. Commit** - `bootloader_state_save_metadata()` appends the new
(size, SHA-256, signature, certificate) record; `bootloader_state_log_event(...)`
appends a tamper-chained log entry (each entry's stored hash covers the
previous entry's raw bytes) carrying the event type, transport, `peer_ip`
(TCP only - always `0` for CDC, USB carries no address-equivalent identity
to log), `HAL_GetTick()` at the time of the event, and the current
`iap_auth` challenge counter (ties an `AUTH_FAIL`/`SIG_FAIL` entry to a
specific challenge attempt instead of just "a failure happened at some
point"). Device reboots into Step 0/1, which re-verifies from scratch
rather than trusting this session's own success report.

## One key, and what a certificate adds

There is exactly one kind of secret in this system: an ECDSA P-256 private
key, and it is never on a device. Everything the board holds is public - a
root public key, and whatever certificates arrive with a command.

Both questions above are answered against that one root:

- **May this caller start an update** (Steps 3, 4) - it presented a
  certificate the root vouches for and signed a fresh nonce with the key that
  certificate names.
- **May this image run** (Steps 1, 6) - the same certificate's leaf key signed
  the image.

A certificate is what lets those two be *different people's* keys over time
without the board learning anything new. The root holder can sign firmware
directly (a self-signed certificate: leaf equals root), or issue certificates
to colleagues so they can upload without ever holding the root key. The board
has no branch for the two cases -- it only ever asks whether the root it
trusts signed the certificate in front of it.

Which root that is comes from the owner-record area, not from the compiled-in
key, once a board has been claimed. See `../docs/design/OWNERSHIP.md`.

## Accepted asymmetries (documented so they aren't mistaken for bugs)

- CDC's 1200bps trigger has no auth - requires physical access, a different
  trust tier than network.
- Discovery replies are unauthenticated by design - see Step 2.
- `iap_auth.c`'s nonce state is a single global - only one challenge in
  flight at a time. Concurrent CDC + Ethernet challenge requests would have
  the second overwrite the first's nonce, failing the first session's
  `flash` auth. Availability quirk, not a security hole (a nonce can never
  be double-consumed or accepted after being overwritten).

## The shipped key is public, and stays that way

`fw_signing_key.TEST_ONLY.pem` is committed, so anyone with this repository can
sign an image a factory board accepts. **That is not a defect awaiting a
rotation.** Users write their own PLC programs; uploading one means signing it;
the private key therefore has to be on the user's machine, and the project
ships no per-customer material. A secret vendor key would mean users could only
run firmware the vendor signed. The derivation is in
`../docs/design/OWNERSHIP.md`.

So everything above protects a factory board against corrupted images and
remote injection, and against nothing else. **The board becomes defended when
it is claimed** (`IAPTool takeown`), or when a customer compiles the board
package with their own root (`keys/rotate_keys.sh`). Until one of those
happens, the boot log says so on every start.

There is no second secret. Session authentication used to derive a per-device
HMAC key from a fixed password compiled into every image; anyone who extracted
that password could compute any device's key from its public UID. That whole
scheme is gone - a signed challenge needs no shared secret, so nothing on the
board is worth extracting.

## TODO

- [ ] **Claim every board before it goes into service.** Nothing above
      protects a board that still trusts the published key, and the vendor
      cannot fix that by rotating -- see the section above. This is an
      operational step, not a code change.
- [ ] **Revoking one delegated certificate.** Today the only revocation is
      handing the board to a new root (`IAPTool setowner`), which voids every
      certificate the old root issued -- including the firmware already
      installed, which must be re-uploaded. Naming a single certificate
      instead is requirement C12; the `serial` field exists for it.
- [ ] **Consolidate `iap_auth.c`/`iap_cert.c`/`sha256.c` across the three
      repos** into one shared source instead of hand-synced copies. What
      guards them meanwhile is `check_mirror_sync.py` (case P2), which
      compares the format constants across all three.

Not on this list any more, and deliberately so:

- **Flash Option Bytes (WRP) on the bootloader sector.** Decided 2026-09-04:
  the vendor does not set it. It is the only thing that would stop a Flash
  write primitive from overwriting the bootloader or its embedded root key,
  but clearing it needs physical SWD access, a reflash and a re-lock -- too
  sharp an edge to ship enabled. A customer who wants that protection can set
  it themselves. See `../docs/design/OWNERSHIP.md`.
- **Per-device manufacturing secrets.** Retired: there is no shared secret
  left to replace.

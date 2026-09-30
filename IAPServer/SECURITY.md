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
(size, SHA-256, signature, certificate) record. Device reboots into Step 0/1,
which re-verifies from scratch rather than trusting this session's own success
report.

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
key, once a board has been claimed. See `$PROD/docs/modules/M2-ownership.md`.

## Accepted asymmetries (documented so they aren't mistaken for bugs)

- CDC's 1200bps trigger has no auth - requires physical access, a different
  trust tier than network.
- Discovery replies are unauthenticated by design - see Step 2.
- **Revoking a leaf stops future uploads; it does not stop firmware that leaf
  already signed.** Handing the board to a new root (`setowner`) does stop it,
  on the very next reset. The two are deliberately different: revocation asks
  whether a *person* is still trusted, and a colleague leaving should not stop
  the boards they once touched, whereas a change of owner asks whose *board*
  this is. `getapprevoked` answers whether the installed image's signer has
  been revoked, which is how an operator finds the boards wanting a re-upload.
  See `$PROD/docs/tables/DECISIONS.md` decisions 60 and 63.
- **`sha256.c`, `iap_cert.c` and part of `iap_auth.c` exist twice**, once
  here and once in the Arduino library, because the two are separate builds
  with no shared source. Not a latent divergence: case P2 compares the first
  two byte for byte, and for `iap_auth.c` it compares the shared functions plus
  the bytes a challenge signature covers. The Go tool is not a third copy -- it
  uses Go's standard library, and cases T1-19/T1-20 cross-check the two
  implementations against each other.
- `iap_auth.c`'s nonce state is a single global - only one challenge in
  flight at a time. Concurrent CDC + Ethernet challenge requests would have
  the second overwrite the first's nonce, failing the first session's
  `flash` auth. Availability quirk, not a security hole (a nonce can never
  be double-consumed or accepted after being overwritten).

## A factory board has no root

Decision 72 (`$PROD/docs/tables/DECISIONS.md`): no root key is compiled into
the bootloader and none is shipped. A factory board trusts no key, so it runs
nothing and accepts nothing until the first upload claims it for the uploader's
own key. Before that claim, whoever reaches the board first can claim it; a
factory reset (BOOT0, ten seconds) puts it back. The derivation is in
`$PROD/docs/modules/M2-ownership.md`.

There is no second secret. Session authentication used to derive a per-device
HMAC key from a fixed password compiled into every image; anyone who extracted
that password could compute any device's key from its public UID. That whole
scheme is gone - a signed challenge needs no shared secret, so nothing on the
board is worth extracting.

## TODO

- [ ] **Claim every board before it goes into service.** Until the first
      upload claims it, anyone who reaches the board first can. This is an
      operational step, not a code change.

Not on this list any more, and deliberately so:

- **Flash Option Bytes (WRP) on the bootloader sector.** Decided 2026-09-04:
  the vendor does not set it. At RDP level 0 or 1 software can clear it
  again (RM0433 section 4.5.2), so it would not stop code already running on
  the board. A customer who wants it can set it themselves. See
  `$PROD/docs/modules/M2-ownership.md`.
- **Per-device manufacturing secrets.** Retired: there is no shared secret
  left to replace.

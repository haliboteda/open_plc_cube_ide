# Firmware signing keys

This project is open source, so the code that verifies a firmware signature
is public -- that is fine, and expected. What must never be public is the
**private** key that produces valid signatures. This directory ships a
clearly-marked placeholder keypair so the project builds out of the box;
you must replace it before shipping anything real.

## What's here

- `fw_signing_key.TEST_ONLY.pem` / `fw_signing_key.TEST_ONLY.pub.pem` --
  a placeholder ECDSA P-256 keypair. The private key is **public** (it's
  committed to this repo), so anyone can produce a signature that the
  placeholder public key (`IAPServer/fw_pubkey.c`) will accept. Fine for
  bring-up and testing on your bench; never acceptable for a device that
  leaves your desk.
- `generate_keys.sh` -- generates a fresh keypair and prints the C array to
  paste into `IAPServer/fw_pubkey.c`.
- `sign_firmware.sh` -- signs a compiled `.bin` for release, producing the
  `(sha256, signature, size)` triple the IAP transfer tool sends to a device
  alongside the image.

## Before building anything for real deployment

1. Run `./generate_keys.sh my_release_key` (pick your own name). This writes
   `my_release_key.pem` (private) and `my_release_key.pub.pem`, and prints a
   C byte array.
2. Paste that array into `IAPServer/fw_pubkey.c`, replacing the placeholder.
   Rebuild the bootloader with the new public key baked in.
3. Move `my_release_key.pem` **off this machine and out of this repo** --
   password manager, offline machine, HSM, whatever your release process
   can keep away from git and away from the network. Losing it means you
   can never sign an update again; leaking it means anyone can.
4. Delete or ignore the placeholder `fw_signing_key.TEST_ONLY.*` files in
   your own deployment if you want (they don't need to exist for this to
   work -- they're only here so a fresh clone builds and boots without
   extra setup).
5. When you cut a release, run
   `./sign_firmware.sh app.bin my_release_key.pem` and feed the resulting
   `.sha256`/`.sig`/`.size` into your release/upload process.

## Why this key is separate from the per-device HMAC key

The IAP protocol has two different keys doing two different jobs (see the
project's security design notes if you have them):

- **This ECDSA keypair** decides whether the bootloader trusts a firmware
  image enough to execute it. One keypair for the whole product/release
  line; the private key never touches a device.
- **Per-device HMAC secrets** (provisioned separately, at manufacturing
  time -- not part of this repo) gate who is allowed to *start* an update
  session at all. Each device gets its own secret.

Keeping these separate matters: if a device is physically compromised and
its HMAC secret extracted, the attacker still cannot forge a firmware
signature, because the ECDSA private key was never on the device in the
first place.

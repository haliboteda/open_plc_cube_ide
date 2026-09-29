# IAP keys

Everything the IAP path treats as a secret lives in this directory. The code
that verifies a signature is public and that is fine; the private key that
produces valid signatures is not.

## What's here

| File | Holds | Committed? |
| --- | --- | --- |
| `fw_pubkey.inc` | The firmware signing **public** key | yes - public by design |
| `fw_signing_key.TEST_ONLY.pem` | Placeholder signing **private** key | yes - see below |
| `fw_signing_key.pem` | Your real signing private key | **no** (gitignored) |
| `*.pem.cert` | A certificate somebody else's root issued for that key | **no** - site-specific |
| `rotate_keys.sh` | Replaces the signing keypair | yes |
| `backup/` | Snapshots taken before each rotation | **no** (gitignored) |

Anything derived from a key sits beside it under the key's own name plus a
suffix. That is not decoration: it makes pairing a key with the wrong
counter, or the wrong certificate, impossible to express.

`fw_pubkey.inc` is `#include`d directly by `fw_pubkey.c`. No generator step, no
generated headers - editing it and rebuilding is the whole mechanism.

The board holds **no shared secret at all**. Session authentication is a
signed challenge under a certificate (see `../iap_cert.h`), so everything
compiled into the firmware is public: a root public key and nothing else.

## Changing the signing key

```sh
./rotate_keys.sh              # new keypair everywhere it is needed
./rotate_keys.sh --dry-run    # show what would change, write nothing
```

It finds the Arduino package and IAPTool by itself, backs up every file it is
about to replace, and writes the new private key to all the places that need a
copy. Useful options:

| Option | Effect |
| --- | --- |
| `--tools=<dir>` | Point at an Arduino install it did not find |
| `--list-backups` / `--restore=<stamp>` | Roll a rotation back |

It needs only a POSIX shell and the `IAPTool` binary that already ships in the
Arduino package. On Windows the package's `busybox.exe sh` runs it - no
openssl, no bash, no Go toolchain.

## After rotating: three steps that are not optional

1. Rebuild the bootloader. The public key is compiled in.
2. **Flash it** over ST-Link or DFU, or over the network with
   `IAPTool flashboot`. `flashboot` needs the image signed by the root the board
   trusts *now* (the old one), and BOOT0 held on an unclaimed board - see
   `$PROD/docs/modules/M1/FLASHBOOT.md`.
3. Rebuild and upload your sketch.

Until step 2 is done on a given board, that board still trusts the old key and
will refuse anything signed with the new one. This is why `rotate_keys.sh` asks
for confirmation.

A board that has been **claimed** does not follow this key at all: it verifies
against the owner recorded in its own flash. Hand such a board over with
`IAPTool setowner`, not by rotating here. See `$PROD/docs/modules/M2-ownership.md`.

## Certificates

A certificate says "this root authorises that key". `IAPTool cert <leafPubHex>`
issues one; the holder saves it as `<their key>.pem.cert` and can then upload
without ever having the root private key. Self-signing (no argument) is what
one person with one key gets, and the board cannot tell the two apart - it only
ever asks whether the root it trusts signed the certificate in front of it.

Certificates carry no serial number. A revocation names the leaf by its own
public key, so nothing here has to be numbered or kept unique - see
`$PROD/docs/modules/M2-ownership.md`.

⚠️ **Rotating the signing key invalidates every certificate issued by the old
one.** `rotate_keys.sh` renames them out of the way rather than leaving them to
fail later with a message that points at the board.

## Who this script is for

**Not the vendor.** The key shipped with this project is public and has to stay
that way: users write their own PLC programs, uploading one means signing it,
so the private key has to be on the user's machine — and the project ships no
per-customer material. Rotating it would produce another key that also has to
ship publicly. The full argument is in `$PROD/docs/modules/M2-ownership.md`; the
short version is that a secret vendor key means users can only run firmware the
vendor signed, which is not this product.

So a factory board is undefended, by construction. The signature guards against
corrupted images and remote injection — **not** against the person holding the
board.

**`rotate_keys.sh` is for a customer who compiles the board package
themselves.** Running it bakes their own root into the bootloader, and their
boards are safe from the network out of the box: nobody else's signature will
start firmware on them, and the "trusts the PUBLISHED root key" warning never
appears. Their owner area is still empty, though, so someone holding BOOT0 can
still `takeown` them - physical access is not defended either way.

A customer who uses the packaged binaries instead gets the same protection with
`IAPTool takeown`, which needs no rebuild and no ST-Link. That is the path most
people are on.

Either way, keep the private key: losing it means you can never sign an update
for those boards again, leaking it means anyone can.

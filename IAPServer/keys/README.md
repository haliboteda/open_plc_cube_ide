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
| `*.pem.certserial` | Certificate serial counter for that key | yes - not a secret |
| `rotate_keys.sh` | Replaces the signing keypair | yes |
| `backup/` | Snapshots taken before each rotation | **no** (gitignored) |

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
2. **Flash it over ST-Link or DFU.** IAP writes the application region only, so
   it can never update the bootloader that holds the root key.
3. Rebuild and upload your sketch.

Until step 2 is done on a given board, that board still trusts the old key and
will refuse anything signed with the new one. This is why `rotate_keys.sh` asks
for confirmation.

A board that has been **claimed** does not follow this key at all: it verifies
against the owner recorded in its own flash. Hand such a board over with
`IAPTool setowner`, not by rotating here. See `../../docs/design/OWNERSHIP.md`.

## The serial counter

`IAPTool cert` numbers each certificate it issues from `<key>.pem.certserial`,
a plain integer kept beside the private key it belongs to. Two certificates
from one root must never share a number, because that number is what a future
revocation list will name. Losing the file restarts the count and can reissue a
number already used by that same root - keep it with the key.

## Before shipping

The placeholder signing key in this repo is public - anyone who cloned the
project can sign an image the placeholder `fw_pubkey.inc` accepts. Run
`./rotate_keys.sh`, then move `fw_signing_key.pem` off this machine: losing it
means you can never sign an update again, leaking it means anyone can.

Note the deliberate trade-off already baked into this design: the signing key
ships inside the Arduino package, because users compile their own PLC programs.
That makes it effectively public. The signature guards against corrupted images
and remote injection - **not** against the person holding the board. Claiming a
board (`IAPTool takeown`) is what closes that gap for a specific customer.

# IAP keys

Everything the IAP path treats as a secret lives in this directory. The code
that verifies a signature is public and that is fine; the private key that
produces valid signatures is not.

## What's here

| File | Holds | Committed? |
| --- | --- | --- |
| `iap_fixed_password.txt` | The shared HMAC password | yes - placeholder |
| `fw_pubkey.inc` | The firmware signing **public** key | yes - public by design |
| `fw_signing_key.TEST_ONLY.pem` | Placeholder signing **private** key | yes - see below |
| `fw_signing_key.pem` | Your real signing private key | **no** (gitignored) |
| `rotate_keys.sh` | Replaces both secrets | yes |
| `backup/` | Snapshots taken before each rotation | **no** (gitignored) |

`iap_fixed_password.txt` and `fw_pubkey.inc` are `#include`d directly by
`iap_keyderive.c` and `fw_pubkey.c`. No generator step, no generated headers -
editing a file here and rebuilding is the whole mechanism. Keep the quotes in
the password file: it is a C string literal.

## Changing the secrets

```sh
./rotate_keys.sh              # new random password + new keypair
./rotate_keys.sh --dry-run    # show what would change, write nothing
```

It finds the Arduino core and IAPTool by itself, backs up every file it is
about to replace, and writes the new password and private key to all the places
that need a copy. Useful options:

| Option | Effect |
| --- | --- |
| `--password=<text>` | Use your own password instead of a generated one |
| `--password-only` | Leave the signing key alone |
| `--key-only` | Leave the password alone |
| `--core=<dir>` / `--tools=<dir>` | Point at an Arduino install it did not find |
| `--list-backups` / `--restore=<stamp>` | Roll a rotation back |

It needs only a POSIX shell and the `IAPTool` binary that already ships in the
Arduino package. On Windows the package's `busybox.exe sh` runs it - no
openssl, no bash, no Go toolchain.

## After rotating: three steps that are not optional

1. Rebuild the bootloader. The password and the public key are compiled in.
2. **Flash it over ST-Link or DFU.** IAP writes the application region only, so
   it can never update the bootloader that holds these secrets.
3. Rebuild and upload your sketch.

Until step 2 is done on a given board, that board still holds the old password
and will not answer IAPTool. This is why `rotate_keys.sh` asks for confirmation.

## The two secrets do different jobs

- **`fw_signing_key.pem` (ECDSA P-256)** decides whether the bootloader will
  *execute* an image. One keypair for the whole product line. The private half
  is never on a device - only signatures are.
- **`iap_fixed_password.txt` (HMAC-SHA256)** decides who may *start* an update
  at all. Mixed with each chip's 96-bit UID to derive a per-device key.

Keeping them separate matters: a password lifted out of one chip lets an
attacker open update sessions, but still not produce firmware that boots.

## Before shipping

The placeholder signing key in this repo is public - anyone who cloned the
project can sign an image the placeholder `fw_pubkey.inc` accepts, and the
password still reads `IAP-FIXED-PASSWORD-TEST-ONLY-CHANGE-ME`. Run
`./rotate_keys.sh`, then move `fw_signing_key.pem` off this machine: losing it
means you can never sign an update again, leaking it means anyone can.

Note the deliberate trade-off already baked into this design: the signing key
ships inside the Arduino package, because users compile their own PLC programs.
That makes it effectively public. The signature guards against corrupted images
and remote injection - **not** against the person holding the board.

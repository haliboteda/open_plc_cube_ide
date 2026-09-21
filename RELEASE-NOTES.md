# Release notes

Covers the whole product, not just this repository: the bootloader lives here,
the Arduino core package and `IAPTool` ship from their own repositories, and a
release is only meaningful as a matching set of all three.

Read [Upgrade rules](#upgrade-rules) before flashing anything onto a board that
already has firmware on it.

---

## Unreleased — 0.1.3

### What changed

- **A rejected upload no longer disturbs the application already on the board.**
  Images are now staged in external SDRAM and fully verified — CRC, hash and
  signature — before the application region is erased. Previously the region was
  erased first, so any image that failed verification left the board reporting
  `BOOTLD-INVALID` until it was reflashed. Verified on hardware: a deliberately
  mis-signed image is refused with `Application region untouched.` and the
  existing application still starts after a reset.
- **Every board now derives its own MAC address from the chip UID.** The
  bootloader previously used a compile-time constant, identical on every unit,
  so two boards on one LAN collided immediately. Devices are still located by
  UID over discovery, not by address, so an application remains free to set its
  own MAC.
- **Discovery rate limiting is device-wide, and the application side has it at
  all.** It used to be per-source-IP on the bootloader only, which meant the
  IDE's discovery helper and `IAPTool` on the same machine drew from one quota
  and knocked each other out roughly once every fifteen requests — reported as
  an intermittent `No response, exiting`.
- **Replay protection across a bootloader handover is fixed.** The bootloader's
  VBAT witness register collided with the application's nonce counter, so every
  pass through the bootloader reset that counter and the application then
  reissued the same nonce sequence. The witness has moved to a free register.
- **The bootloader reports its own version correctly** (`0.1.3`; it said `0.1.2`
  before) and reports the reset cause as `PIN`, `SOFT` or `POR`.
- **New journal event** for "image verified but the flash write failed",
  distinct from a signature failure — the two need different diagnosis.

- **Every sketch must now declare its own version.** Add one line near the top:
  `OPENPLC_APP_VERSION(1, 0, 0);`. A sketch without it **does not compile** —
  this is deliberate, the upload tool needs a version to compare against.
  Existing sketches will fail on their first build with the new package and the
  error says which line to add.
- **Uploads refuse to go backwards.** If the image is older than what the board
  reports running, the upload stops. To flash it anyway set
  *Tools ▸ Force flash (allow older version)* to **Yes**; that is **one-shot**
  and is refused again until the menu goes back to **No**.
  ⚠️ **This only works over ETH Transfer.** On CDC the board running an
  application answers nothing (the sketch owns the CDC pipe), so its version
  cannot be read; SWD and Serial bypass the tool entirely. The check is a guard
  against pushing a stale build by mistake — it is not a security control.
- **The board reports two versions now.** The discovery reply gained a fifth
  field: `name_uid_role_<package version>_<sketch version>`. The bootloader has
  no sketch version to report and sends `-`.
- **The event log is gone.** The eight journal events supported no requirement
  and nothing ever read them back. The state sector now holds firmware metadata
  only, with its first 8 KiB reserved for calibration data.

### Upgrade rules

> **The bootloader and the application must be upgraded together. This is not a
> recommendation.**

Application metadata (size, hash, signature) lives in the last flash sector,
and 0.1.3 changed its layout more than once. The current layout reserves the
first 8 KiB of that sector for calibration data and starts the metadata area
after it, so a record written by any earlier build no longer lines up. Neither
does a journal written by 0.1.2. In every one of those cases:

1. It finds no metadata for the application already in flash.
2. It therefore declares that application invalid.
3. It stays in the bootloader and reports role `BOOTLD-INVALID`.

The application image itself is untouched and byte-for-byte fine. What is gone
is the bootloader's record that it was ever installed.

**The board looks bricked. It is not.** It still answers discovery over both USB
CDC and Ethernet, and it still accepts uploads. Upload the sketch again and the
new bootloader writes fresh metadata in the format it understands.

**What it will not do is recover on its own.** Ship the two images as one
bundle, and never let a field board take a bootloader update alone.

There is no code-level mitigation for this, by decision. The old format is not
readable and adding a compatibility path would mean carrying a parser for a
format no released board is supposed to keep.

**The event log goes quiet until that first upload, too.** The boot after a
bootloader upgrade reports an unrecognised record and a full journal, and stops
recording events — it will not erase a sector on the strength of a record it
cannot read. The next successful update reclaims the sector, and logging
resumes. Observed going from a full 4096 slots to 10 on the upload that
followed.

### Upgrade in this order

**Board package first, bootloader second.** The intermediate state is usable
that way round: a sketch built with the new package runs fine on an old
bootloader (the metadata format it writes has not changed, and the extra
identity field only makes an old tool's display untidy). The other way round
leaves the board sitting in `BOOTLD-INVALID` until the application is re-sent.

Either way the application has to be uploaded once after the bootloader is
replaced — see [Upgrade rules](#upgrade-rules).

### The upload tool has to be upgraded too

**A 0.1.2 IAPTool cannot flash a 0.1.3 board**, and the failure is quiet: the
old tool authenticates with an HMAC, the new bootloader wants a certificate and
a signed challenge, and the board answers `ERR` with
`flash command failed authentication` on its serial log. Nothing says "wrong
tool version".

The tool ships inside the Arduino board package, so installing the package
updates it — but a machine that keeps a copy elsewhere, or a script pointing at
an old path, will fail this way until it is pointed at the new one.

### Flashing the bootloader

IAP writes the application region only — it can never update the bootloader.
Use ST-Link or DFU.

> **Reflashing the bootloader also resets ownership.** The owner records live in
> the top 8 KB of the bootloader's own flash sector, so erasing that sector to
> write a new bootloader takes them with it. That is semantically right — anyone
> who can attach ST-Link could reset the board anyway — but it stacks on top of
> the rule above: **whoever replaces a bootloader on a claimed board must
> re-upload the application *and* claim the board again.**

## Board ownership

A board verifies firmware against a root key. Out of the factory that is the key
published with this project, and its private half is in the repository, because
customers have to be able to sign their own sketches. So a factory board will
run firmware signed by anybody, and it says so on every boot:

```
** This board trusts the PUBLISHED root key: anyone can sign firmware it will run. **
```

Claiming the board binds it to a key of your own, after which it runs nothing
else. Three operations:

| Operation | How it is authorised |
|---|---|
| **Claim** (`takeown`) | Hold BOOT0 through the startup window. There is no owner yet to sign anything, so physical presence is the only possible gate — and until a board is claimed, whoever gets there first wins |
| **Change owner** (`setowner`) | The current owner's signature. No button: signing *is* the authorisation, and handing a board over remotely is supported |
| **Factory reset** | Hold BOOT0 for ten seconds after reset, until three rapid relay clicks, then release. Back to the published root, and claimable again |

**Factory reset deliberately needs no signature.** Requiring the current owner's
would leave a customer who lost their private key with a board only ST-Link could
rescue — and that customer is exactly the one without an ST-Link. The cost is
that anybody who can physically reach a board can reset it and take it over;
what it buys is that nobody can do it remotely.

**Claim a board before putting it into service.** Everything above only starts
protecting anything from the moment it is claimed.

`IAPTool` drives all three:

```
IAPTool genkey owner                 writes owner.pem
IAPTool getowner <ip>                which key the board trusts, at which generation
IAPTool takeown  <ip> --key=owner.pem
IAPTool setowner <ip> --current-key=owner.pem --new-key=next.pem
```

`takeown` needs BOOT0 held through the board's current boot; `setowner` needs
only the current owner's key, so a handover can be done remotely. `takeown`
refuses to fall back to the signing key from `local_config.json` — claiming a
board with the wrong key can only be undone with an ST-Link.

### Letting colleagues upload without the owner key

One person with one key needs nothing here. A team where one administrator
holds the owner key does: each colleague keeps a key of their own, and the
administrator issues a certificate saying that key is authorised.

```
colleague:      IAPTool pubkey keys/fw_signing_key.pem      → 128 hex characters
administrator:  IAPTool cert <those characters> --key=owner.pem
colleague:      save the reply as keys/fw_signing_key.pem.cert
```

Uploading is unchanged from there, including from the Arduino IDE — the
certificate is found beside the key it covers. **The owner private key never
leaves the administrator's machine.**

**Revoking a colleague means handing the board to a new owner** (`setowner`)
and issuing fresh certificates to everyone still there. Certificates from the
old owner stop verifying the moment the board's owner changes — including on
firmware already installed, which the board will refuse at the next reset until
someone uploads again. Plan a revocation as a maintenance window, not as a
click.

### Known issues

- **If the start-up log says `** SDRAM SELF-TEST FAILED at offset ... **`, stop
  and fix the hardware before testing anything else.** Images are staged in
  external SDRAM and verified there before flash is touched, so an unusable
  staging buffer means every upload stops at the checksum — over Ethernet and
  over USB alike. **The board stays safe** (the application region is never
  written, and whatever is installed keeps running), but no upgrade completes.

  If a board shows this, the external SDRAM is the first thing to suspect: the
  diagnostic in `TestCase/SDRAM/sdram_test.c` walks the data bus from inside the MCU
  and names the failing line.
- **The application's `[BOOT] millis=` banner reaches the RS232 terminals only
  by accident of timing.** The core prints it before anything drives the
  transceiver enable high, so the MAX3221 is nominally shut down — but its
  charge pump still holds enough residual voltage to produce valid RS-232 levels
  for a few milliseconds, and the banner slips out on that. Measured at
  `millis=12`. **Nothing guarantees that window.** An application that starts
  more slowly will lose the line entirely, so do not build anything on it and do
  not add further start-up printing at that point in the core.
- **Discovery rate limiting is a fixed window, not a token bucket.** Nominally
  50 replies/sec; a burst straddling a window boundary has been measured at 60.
  Treat 50 as approximate, not as a guarantee.
### Not verified

- **MAC addresses are unique across boards.** Derivation from the chip UID
  replaced the hardcoded `00:80:E1:00:43:21`, and one board was confirmed to
  derive and use its own address consistently in both the bootloader and the
  application. Only one board was available, so uniqueness *between* boards has
  never been observed. Put it on the production checklist.
- ~~Behaviour when power is lost mid-upgrade.~~ **Verified 2026-09-01** (test
  cases T1-21/T1-22): losing power during the transfer is harmless — the old
  application starts normally and the application region is untouched. Losing
  power during the erase/write window leaves the board reporting `metadata
  present` with `App signature invalid or absent`, and re-uploading recovers
  it. Measured pull-safe windows: **33.7 s** during transfer, **20.2 s** during
  erase/write.

  **That erase/write window is wider and easier to hit than assumed** — the
  upload tool exits as soon as it has sent the last byte, while the board is
  still verifying, erasing and copying out of SDRAM. Anything automating an
  upgrade should wait for the board's own `Checksum and signature OK` rather
  than for the tool to exit.
- Long-term stability under a real OpenPLC runtime.

### The shipped signing key is public on purpose

Its private half is committed, so anyone with the source can sign an image a
factory board accepts. **Rotating it is not the answer, and the vendor cannot
be the one to do it.** Users write their own PLC programs; uploading one means
signing it; the key therefore has to be on the user's machine. A secret vendor
key would mean users could only run firmware the vendor signed.

A board becomes defended when it is claimed (`IAPTool takeown`, no ST-Link
needed), or when a customer compiles the board package with a root of their own
(`IAPServer/keys/rotate_keys.sh`). Until then the boot log says on every start
that anyone can sign firmware it will run. See `IAPServer/keys/README.md`.

There is no second secret: the board holds only public keys.

---

## Release checklist

Everything here has burned someone at least once.

The first three are now checked by `$TOOL/TestCase/tools/selfcheck.py`, which
also runs the host-side tests. Run it before working through the rest by hand.

- [ ] `OPENPLC_FW_VERSION` in `Core/Inc/IAP_config.h` matches
      `OPEN-PLC.build.fw_version` in the core package's `boards.txt`. Nothing
      enforces this — the two live in different repositories with no shared
      build. → `$TOOL/TestCase/tools/check_version_sync.py`
- [ ] Every mirrored file is in sync across the three repositories (see
      `$PROD/docs/repo/ARCHITECTURE.md`, "跨仓镜像的代码"). A divergence does not fail the
      build; it shows up at runtime as something unrelated.
      → `$TOOL/TestCase/tools/check_mirror_sync.py`
- [ ] Everything verified in the live Arduino15 package has been copied back
      into the core package's git repository and committed.
      → `$TOOL/TestCase/tools/check_core_sync.py`
- [ ] The published-root warning still fires on an unclaimed board
      (`$TOOL/TestCase/tools/check_public_root.py`, case T2-06). It is the only
      thing telling a customer their board is undefended, and it goes quiet
      the moment the fingerprint it compares against drifts.
- [ ] Bootloader flashed over ST-Link/DFU and the application uploaded over
      IAP, in that order, on a board that previously ran the older release.

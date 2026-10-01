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
- **The bootloader can now be replaced over the network**, with
  `IAPTool flashboot <boot.bin> <ip> --key=<owner.pem>`, and the board stays
  claimed: the owner records are carried across the erase, and compacted while
  they are out of the way. The key must be the owner root -- a leaf certificate
  authorises applications, not bootloaders -- and an unclaimed board demands
  BOOT0 held through start-up instead. Verified on hardware: five bootloader
  replacements in a row, ownership and the installed application intact after
  each one.
  ⚠️ **Do not cut power during it.** The board is running out of the sector
  being rewritten. If it is interrupted the board will not start; hold BOOT0
  through a reset to reach the ST ROM DFU and re-flash over USB. Whether
  ownership survived depends on where it stopped, and the boot log says so.
  ⚠️ Re-flashing over **ST-Link still wipes ownership** -- the owner records
  live in the bootloader's own sector. Use `flashboot` to keep it.
  ⚠️ Going *into* this release still loses ownership whichever way you do it:
  the owner record format changed, so no record written by an earlier build is
  read. From 0.1.3 onwards `flashboot` keeps it.
- **A leaf certificate can be revoked**, so one departing colleague no longer
  means re-keying everybody:

  ```sh
  IAPTool revoke <ip> --key=<owner.pem> --leaf=<the leaf's 128-hex public key>
  ```

  Signed by the current owner, so no button and no site visit. Revoking the
  same leaf twice is free -- the board answers `OK already revoked` and writes
  nothing, which is also what makes a replayed request harmless.
  ⚠️ **Firmware that leaf already signed keeps running.** Revoking blocks the
  next upload, not the machines already in service -- a colleague leaving
  should not stop the lines they once commissioned. Ask a board whether it is
  affected with `IAPTool getapprevoked <ip>`, and re-upload at your
  convenience.
  ⚠️ **The root can never revoke itself**, by rule: an entry naming the root in
  force is ignored rather than honoured.
- **`IAPTool getapprevoked <ip>`** answers whether that board's installed
  firmware was signed by a leaf that has since been revoked: `REVOKED`, `OK`,
  or no signed firmware at all. One board per call -- run it over the boards
  discovery finds to draw up the re-upload list.
- **The owner record area holds 96 revocations**, in their own fixed segment
  alongside 32 ownership records. The boot line reports both:
  `Owner slot: 31/32 owner slot(s) free, 96/96 revoke slot(s) free`.
  The area only ever appends, so the board starts warning with 8 revocation
  slots left.
  ⚠️ **Changing the root does not free those slots.** It retires every leaf the
  old root issued -- so they need no individual revocation -- but the records
  already written stay where they are. Only `setowner --wipe`, a `flashboot`,
  or an ST-Link reflash reclaims them.
- **`setowner --wipe` hands the board over and empties the record area**, which
  is the only way to get revocation slots back without an ST-Link:

  ```sh
  IAPTool setowner <ip> --current-key=<a.pem> --new-key=<b.pem> --wipe
  ```

  The board erases and rewrites its own flash sector to do it, so it resets.
  ⚠️ **Same power rule as `flashboot`**: a cut during the erase means holding
  BOOT0 through a reset and re-flashing over USB DFU.
  ⚠️ **After a wipe the board's ownership can no longer be proved back to the
  factory.** Each ownership record is authorised by the one before it, and a
  wipe discards them; the record it writes therefore carries no signature and
  reads as a first claim. Nothing is weakened -- the signature authorising the
  wipe is checked before a byte is erased, and it covers both the generation
  and that board's UID -- but the chain is gone. Plain `setowner` keeps it.
- **The upload tool no longer reports a refused image as a successful upload.**
  The board checks the image signature only once it has the whole image, and
  `IAPTool` used to stop listening before that verdict arrived: an image the
  board threw away was reported as `File transfer complete.` with exit code 0.
  It now reads the verdict, and confirms a success by watching the board come
  back on the network.
- **The event log is gone.** The eight journal events supported no requirement
  and nothing ever read them back. The state sector now holds firmware metadata
  only, with its first 8 KiB reserved for calibration data.

### Upgrade rules

> **The bootloader and the application must be upgraded together. This is not a
> recommendation.**

Application metadata (size, hash, signature) lives in the last flash sector,
and 0.1.3 changed its layout more than once. The current layout reserves the
first 8 KiB of that sector for calibration data and starts the metadata area
after it, so a record written by any earlier build no longer lines up. In every
one of those cases:

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

**The boot after a bootloader upgrade reports the metadata area as full**, for
the same reason: it will not erase a sector on the strength of records it cannot
read. The next successful upload reclaims it, carrying the calibration area
across, and the count drops back. Observed on hardware 2026-09-21.

### The upgrade into 0.1.3 needs somebody at the board

This applies **once**, to the upgrade into this release, and not to any
upgrade after it.

An application built before 0.1.3 cannot reboot itself into the bootloader on
a 0.1.3 board. It reads the owner records to decide who may ask it to reboot,
and it does not understand the layout this release introduced, so it falls
back to the published root and refuses a request signed by the board's real
owner. The board is fine and still answers discovery — it just will not take
that one command.

**So the upgrade into 0.1.3 needs BOOT0 held through a reset**, unless the
board is already sitting in its bootloader. Once a 0.1.3-built application is
installed, `IAPTool` can drive the whole cycle over the network again.

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

There are two ways, and they differ in what survives.

| | `IAPTool flashboot` | ST-Link / DFU |
|---|---|---|
| Ownership | **kept** | **erased** |
| Installed application | kept | kept (the image itself is never touched) |
| Needs a cable at the board | no | yes |
| Needs the owner's private key | yes | no |

`flashboot` is the normal route from 0.1.3 onwards. ST-Link remains the way in
when there is no owner key to sign with, and the way back when something went
wrong.

> **Reflashing over ST-Link resets ownership.** The owner records live in the
> top 8 KB of the bootloader's own flash sector, so erasing that sector to write
> a new bootloader takes them with it. That is semantically right — anyone who
> can attach ST-Link could reset the board anyway — but it means **whoever
> replaces a bootloader that way on a claimed board must claim it again**, and
> re-upload the application because the new owner cannot verify the old one's.

## Board ownership

A board verifies firmware against a root key it keeps in flash, outside the
bootloader code. A factory board has no root at all: it runs nothing and
accepts nothing until it is claimed. The first upload claims it:

```
This board has no root yet: claiming it for this computer's key.
Claimed. From now on this board runs only firmware signed by <path>
```

IAPTool takes the key it finds (see below) or generates one in the default
location, prints where it is, and binds the board to it -- over USB or
Ethernet, no button, no ST-Link. Keep that file: it is the only key the board
will take firmware from. Four operations:

| Operation | How it is authorised |
|---|---|
| **Claim** (`takeown`) | Only a board with no root accepts it. The first upload does it automatically; until then, whoever reaches the board first claims it |
| **Change owner** (`setowner`) | The current owner's signature. No button, as often as needed; the bootloader is never rewritten |
| **Revoke a leaf** (`revoke`) | The current owner's signature. Withdraws one delegated certificate without touching the others. The root itself can never be revoked |
| **Factory reset** | Hold BOOT0 for ten seconds after reset, until the system LED stays lit, then release. The board is back to no root, and the next upload claims it again |

**Factory reset deliberately needs no signature.** Requiring the current owner's
would leave a customer who lost their private key with a board only ST-Link could
rescue. The cost is that anybody who can physically reach a board can reset it
and take it over; what it buys is that nobody can do it remotely.

`IAPTool` drives all of them:

```
IAPTool genkey                       writes the default key and prints its path
IAPTool getowner <board>             which key the board trusts, at which generation
IAPTool takeown  <board> --key=owner.pem
IAPTool setowner <board> --current-key=owner.pem --new-key=next.pem
IAPTool setowner <board> --current-key=owner.pem --new-key=next.pem --wipe
IAPTool revoke   <board> --key=owner.pem --leaf=<128 hex characters>
IAPTool getapprevoked <board>        is this board's firmware signed by a revoked leaf?
```

`<board>` is an IP address or the USB port (`COM6`, `/dev/ttyACM0`).
**New computer:** copy the root private key into the default location there.
`--wipe` additionally empties the revocation records; it rewrites sector 15,
not the bootloader.

### Where IAPTool finds the upload key

The Arduino IDE passes no key, so IAPTool uses the first one it finds, and on a board with no root generates one in row 3's location:

| # | Where |
|---|---|
| 1 | `--key=<path>` |
| 2 | `"signing_key"` in `local_config.json` beside IAPTool (write an absolute path) |
| 3 | **`openplc/keys/fw_signing_key.pem` in your user config directory — put your key here.** Windows `%AppData%\openplc\keys\`, macOS `~/Library/Application Support/openplc/keys/`, Linux `~/.config/openplc/keys/`. It survives tool package upgrades |
| 4 | `keys/fw_signing_key.pem` beside IAPTool — older setups; lost when the tool package is upgraded |

A certificate goes beside the key it covers, as `<key>.cert`.
When an Ethernet upload to a running board stops with *the board did not accept
the reboot request*, the key it names is most likely not the one that board
trusts; `IAPTool getowner <ip>` shows which one it does (the bootloader answers
it, the running sketch does not).

### Letting colleagues upload without the owner key

One person with one key needs nothing here. A team where one administrator
holds the owner key does: each colleague keeps a key of their own, and the
administrator issues a certificate saying that key is authorised.

```
colleague:      IAPTool pubkey                              → 128 hex characters
administrator:  IAPTool cert <those characters> --key=owner.pem
colleague:      save the reply beside the key as fw_signing_key.pem.cert
```

Uploading is unchanged from there, including from the Arduino IDE — the
certificate is found beside the key it covers. **The owner private key never
leaves the administrator's machine.**

**Withdrawing one colleague's certificate is now a single command** — see
`IAPTool revoke` above. It names that one leaf and leaves everybody else's
certificates working.

**Changing the root is still the bigger hammer**, and sometimes the right one:
`setowner` retires every certificate the old root issued at once, which is what
you want if the owner key itself is in doubt rather than one colleague's.

**They differ in what happens to firmware already installed**, on purpose:

| | Firmware already on the board | That key uploading again |
|---|---|---|
| `takeown` (first claim) | **Refused at the next reset** — it was signed with the published key. Re-sign it with your own and upload again | Refused |
| `revoke` a leaf | **Keeps running** | Refused |
| `setowner` (change of root) | **Refused at the next reset** | Refused |

`revoke` asks whether a *person* is still trusted; `setowner` asks whose
*board* this is, and a new owner rarely wants the previous one's firmware still
running. So plan a change of root as a maintenance window; a revocation only
needs a re-upload list, which `IAPTool getapprevoked` gives you.

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

  **That erase/write window is wider and easier to hit than assumed**, and it
  used to be worse: the tool exited as soon as it had sent the last byte, while
  the board was still verifying, erasing and copying out of SDRAM. It now waits
  for the board to finish and come back, so the tool's exit is a usable signal
  again and automation no longer has to watch the serial log for
  `Checksum and signature OK`.
- Long-term stability under a real OpenPLC runtime.

There is no second secret: the board holds only public keys.

---

## Release checklist

Everything here has burned someone at least once.

The first three are now checked by `$TEST/tools/selfcheck.py`, which
also runs the host-side tests. Run it before working through the rest by hand.

- [ ] `OPENPLC_FW_VERSION` in `Core/Inc/IAP_config.h` matches
      `OPEN-PLC.build.fw_version` in the core package's `boards.txt`. Nothing
      enforces this — the two live in different repositories with no shared
      build. → `$TEST/tools/check_version_sync.py`
- [ ] Every mirrored file is in sync across the three repositories (see
      `$PROD/docs/repo/ARCHITECTURE.md`, "跨仓镜像的代码"). A divergence does not fail the
      build; it shows up at runtime as something unrelated.
      → `$TEST/tools/check_mirror_sync.py`
- [ ] Everything verified in the live Arduino15 package has been copied back
      into the core package's git repository and committed.
      → `$CORE_REPO/tests/check_core_sync.py`
- [ ] Bootloader flashed over ST-Link/DFU and the application uploaded over
      IAP, in that order, on a board that previously ran the older release.

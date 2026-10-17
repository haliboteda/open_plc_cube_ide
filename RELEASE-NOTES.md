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
  claimed: the owner records live in sector 15, which `flashboot` never
  touches. The key must be the owner root -- a leaf certificate authorises
  applications, not bootloaders -- and a board with no root refuses it (claim
  it first; the first upload does that). Verified on hardware.
  ⚠️ **Do not cut power during it.** The board is running out of the sector
  being rewritten. If it is interrupted the board will not start; re-flash the
  bootloader over ST-Link. Ownership survives either way.
  ⚠️ Going *into* this release still loses ownership: the sector-15 layout
  changed, so nothing written by an earlier build is read (see
  [Upgrade rules](#upgrade-rules)). From 0.1.3 onwards both `flashboot` and an
  ST-Link reflash of the bootloader keep it.
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
  already written stay where they are. Only `setowner --wipe` reclaims them.
- **`setowner --wipe` hands the board over and empties the record area**, which
  is the only way to get revocation slots back without an ST-Link:

  ```sh
  IAPTool setowner <ip> --current-key=<a.pem> --new-key=<b.pem> --wipe
  ```

  The board erases and rewrites sector 15 to do it, so it resets. A power cut
  during the rewrite is finished at the next start from a copy held in backup
  SRAM; the bootloader itself is never touched.
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
- **Every output stays at 0 until the sketch first writes it.** The bootloader
  drives the digital outputs low, the analog outputs to 0 mA and `KNX_TX` low,
  rather than leaving them to whatever a floating pin does. The per-output
  table is in the `OpenPLC_Ports` README, "Outputs at power-up, reset and power
  loss".
- **The boot log warns when the brown-out reset level is not 2.7 V.** The level
  lives in the chip's option bytes; the bootloader only reads it.
- **Logs and example output go to RS232** (terminals C05 / C06, 115200 baud),
  not USB. USB is for uploading; `USB_Serial` is the one example that still
  prints there, because USB serial is what it shows. The core switches the
  RS232 transceiver on at start-up, so a sketch no longer enables it, and the
  `[BOOT] millis=` line no longer depends on start-up timing.
- **`openplcResetCause()`** tells a sketch why the board last reset: watchdog,
  power-on, reset button, software or brown-out. `IWatchdog.isReset()` is always
  false on this board, because the bootloader has already cleared the flag it
  reads. The board package turns on no watchdog and claims no alarm output;
  the `OpenPLC_Ports` README shows how to build both.
- **Analog in mV and mA, with this board's calibration**:
  `openplcReadAI1_mV()`, `openplcReadAI2_mA()`, `openplcWriteAO_mA()` and
  `openplcCalibrationStatus()`. A board without valid calibration falls back to
  the nominal conversion and says so once on the log. `analogRead()` /
  `analogWrite()` stay raw.
- **PWM resolution on the digital outputs.** `DO_PWM` sets duty, frequency and,
  with `r <bits>` (1–16, default 8), the resolution, using the core's
  `analogWriteResolution()`. Outputs pair up on timer channels (DO1/DO5,
  DO2/DO6, DO3/DO7, DO4/DO8): only one of a pair runs PWM. Verified on hardware.
- **SD card and file system ship in the board package**: `OpenPLC_SD` with
  FatFs, an Arduino-style `SD` / `File` interface, FAT32 and exFAT, cards
  partitioned MBR or GPT. It contains no GPL code, so linking it puts no
  obligation on your firmware. `SD_FileReceive` takes a file from the PC over
  USB with YMODEM (`openplcYmodemReceive()`) and writes it to the card.
- **KNX TP is rewritten for the board's STKNX transceiver.** The library times
  the TP1 bits itself in two timer interrupts, including the acknowledge,
  repetitions and collisions, so a slow `loop()` does not miss an acknowledge.
  ⚠️ **A sketch using `OpenPLC_KNX` cannot use TIM1 or TIM12** for anything
  else, PWM on DO1/DO2/DO5/DO6 included.
- **KNX has two roles, both programmable by ETS**: *Tools ▸ KNX Role* offers
  KNX TP Device (mask `0x07B0`, the default) and KNX IP Router (`0x091A`).
  The IP+TP device `0x5780` and the IP-only device `0x57B0` are gone.
  The programming button is BOOT0: press it once while the sketch runs, and
  the system LED shows programming mode. When ETS restarts the board, the log
  says so first (`KNX: restart requested by ETS ...`).
- **KNX examples**: `KNX_Switch`, `KNX_Inputs`, `KNX_Temperature`,
  `KNX_Analog`, `KNX_BusMonitor` (prints every telegram and sends group writes
  typed on RS232, no ETS needed) and `KNX_IPRouter`. `KNX_Basic`,
  `KNX_IP_Test`, `KNX_SelfTest`, `KNX_TP_PingPong` and `KNX_TP_TxTest` are
  removed.
- **Eleven upstream examples that do not fit this board are removed**, from the
  `Keyboard`, `Mouse`, `RGB_LED_TLC59731`, `SPI`, `Servo`, `SoftwareSerial` and
  `SubGhz` libraries.
- **`IAPTool` refuses a board whose bootloader cannot answer `getpubkey`**
  instead of uploading without checking the key. Older bootloaders are not
  supported; flash the 0.1.3 bootloader first (see
  [Boards already in service](#boards-already-in-service-flash-the-bootloader-over-st-link)).
- **An Ethernet upload stops if a different board answers at the address.**
  After rebooting the board into its bootloader, `IAPTool` checks the UID at
  the original IP and refuses with `<ip> now answers as a different board`
  instead of uploading to it.
- **Upload output is short by default.** Without *Show verbose output during
  upload* the IDE shows only the key lines and a progress bar; with it, every
  block and command.
- **The IDE port list relabels a board** when it switches between application
  and bootloader at the same address, instead of keeping the old label.
- **Uploads work on Linux and macOS**: the upload tool, the discovery helper
  and the build scripts install as executables, so an upload no longer fails
  with `Permission denied`.
- **A second compile without changes is incremental** instead of rebuilding
  the whole core.
- **An ETS download can no longer erase the application.** An uninitialised
  erase request in the KNX storage could wipe the flash sector holding the
  application signature, ownership and calibration.
- **The KNX data area is 16 KiB** (about 2,700 group links instead of about
  110), enough for the IP router's filter table. Data from earlier ETS
  downloads stays valid.
- **The ETS product file is no longer in the board package.**
  `OpenPLC_Bridge.knxprod` is removed; the product for this release is
  `OpenPLC_TP.knxprod` (application version 5, order number `OPENPLC-TP`),
  downloaded from [OpenPLC_ETS_Prod](https://github.com/haliboteda/OpenPLC_ETS_Prod).

### Sketches that need editing

These names changed with no alias left behind, so a sketch using the old ones
**does not compile** with this package. One spelling per thing keeps the
interface consistent.

| Was | Now |
|---|---|
| `Serial_Test` | `RS232` |
| `RS232_TX_Pin`, `RS232_RX_Pin`, `RS232_EN_Pin` | `RS232_TX`, `RS232_RX`, `RS232_EN` |
| `KNXHelper.<method>` | `KNX.<method>` — `KNX` is the only KNX object now |
| `KNX.setup("OPENPLC000001")` (serial number) | `KNX.setup("OPENPLC-TP")` — the ETS product's order number |
| *KNX Role* `IP+TP Device` (`0x5780`) or `KNXnet/IP Device` (`0x57B0`) | `KNX TP Device` (`0x07B0`) or `KNX IP Router` (`0x091A`) |
| `#include <STM32SD.h>` | `#include <OpenPLC_SD.h>`; `SD` / `File` keep their shape |
| `SD.begin(SDMMC_CD_Pin)`; `digitalRead(SDMMC_CD_Pin)` | `SD.begin()`; `SD.cardPresent()` — the card-detect pin is built in. `SDMMC_CLK_Pin`, `SDMMC_CMD_Pin`, `SDMMC_D0_Pin` are gone; `SDMMC_CD_Pin` is `SDMMC_CD` |
| `CAN_TX_Pin`, `CAN_RX_Pin` | `CAN_TX`, `CAN_RX` (the `CAN` object sets them up itself) |
| `openplc_reset_cause()` | `openplcResetCause()` |
| `openplc_net_link_up()`, `openplc_net_has_ip()`, `openplc_net_get_ipv4(ip)`, `openplc_net_get_mac(mac)` | `Net.linkUp()`, `Net.hasIP()`, `Net.localIP()` (an `IPAddress`), `Net.macAddress(mac)`; no `#include` needed. The `openplc_lwip_*` statistics are gone |

**How to migrate:** search the sketch for each name in the left column and
replace it; the compiler error names each line still left. `KNX` exposes
`setup`, `start`, `loop`, `progMode`, `configured`, `individualAddress`,
`getGroupObject` (TP device only), `onBusFrame`, `sendGroupWrite` and
`onBusSendDone`; the `OpenPLC_KNX` README describes each. A sketch that called
other `KnxFacade` methods on the old `KNX` has no replacement for them yet.
Lines that switch `RS232_EN` on are no longer needed: the core does it.

### Upgrade rules

> **The bootloader and the application must be upgraded together. This is not a
> recommendation.**

The bootloader keeps its state in the last flash sector (sector 15), laid out
as:

| Address | Size | Holds |
|---|---|---|
| `0x081E0000` | 8 KiB | Calibration data, written at production; never erased by an upgrade |
| `0x081E2000` | 8 KiB | Root area: ownership records and revoked leaves |
| `0x081E4000` | to `0x081FFFE0` | Application metadata (size, signature, certificate), appended once per upload |
| `0x081FFFE0` | 32 bytes | Layout marker, written last, so a half-written sector is recognised |

A sector 15 written by any earlier build has no layout marker. The first start
of the 0.1.3 bootloader therefore prints
`** Sector 15 is not in this layout ... rebuilt with calibration only. **`
and rewrites the sector keeping only the calibration data, so:

1. The board has **no root**: whoever owned it before no longer does.
2. It has **no metadata** for the application already in flash, so it does not
   run it and reports role `BOOTLD-INVALID`.

**The board looks bricked. It is not.** It still answers discovery over both USB
CDC and Ethernet, and it still accepts uploads. Upload the sketch again: that
first upload claims the board for your key and writes fresh metadata.

**What it will not do is recover on its own.** Ship the bootloader and the
application as one bundle, and never let a board take a bootloader update
alone. There is no compatibility path for the old layout, by decision: no
released board is supposed to keep it.

### Boards already in service: flash the bootloader over ST-Link

**Every board running a bootloader older than 0.1.3 gets the 0.1.3 bootloader
over ST-Link.** There is no in-place upgrade from an older bootloader, by
decision: ownership does not carry across the layout change anyway.

1. **Flash the 0.1.3 bootloader over ST-Link** — steps in
   [README.md, "Download BootLoader-Menu to PLC-H743IIKx"](README.md#download-bootloader-menu-to-plc-h743iikx)
   (for a scripted flash that also checks the result:
   `$PROD/docs/engineering/HOW-TO-RUN-TESTS.md`, "出厂态怎么造、怎么证明").
2. **Install the 0.1.3 board package.** The version number is unchanged from
   the earlier 0.1.3 build, so the IDE offers no update: remove the installed
   package and install it again.
3. **Upload the sketch.** The board has no root, so this first upload claims it
   for your key and the board starts the application.

### The upload tool has to be upgraded too

**A 0.1.2 IAPTool cannot flash a 0.1.3 board**, and the failure is quiet: the
old tool authenticates with an HMAC, the new bootloader wants a certificate and
a signed challenge, and the board answers `ERR` with
`flash command failed authentication` on its serial log. Nothing says "wrong
tool version".

The tool ships inside the Arduino board package, so installing the package
updates it — but a machine that keeps a copy elsewhere, or a script pointing at
an old path, will fail this way until it is pointed at the new one.

### Flashing the bootloader from 0.1.3 onwards

Once a board runs the 0.1.3 bootloader, there are two ways to replace it, and
both keep ownership, the application and calibration: the root area and the
metadata live in sector 15, which neither of them writes.

| | `IAPTool flashboot` | ST-Link |
|---|---|---|
| Ownership | kept | kept |
| Installed application | kept | kept |
| Needs a cable at the board | no | yes |
| Needs the owner's private key | yes | no |

`flashboot` is the normal route. ST-Link is the way back when a `flashboot` was
interrupted, and the way in when there is no owner key to sign with.
⚠️ **A full-chip erase does clear ownership** (and the application): the board
is back to no root, and the next upload claims it.

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
| `takeown` (first claim) | None runs: a board with no root runs nothing. The upload that claims it installs the first | — |
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
- **Discovery rate limiting is a fixed window, not a token bucket.** Nominally
  50 replies/sec; a burst straddling a window boundary has been measured at 60.
  Treat 50 as approximate, not as a guarantee.

### Not verified

- ~~MAC addresses are unique across boards.~~ **Verified 2026-09-28** on two
  boards: each derives its own address from the chip UID.
- **KNX on a real bus with ETS.** The TP data link passed its host-side tests,
  and `KNX_BusMonitor` sent and received on a real bus; the ETS-programmed
  examples (`KNX_Switch`, `KNX_Inputs`, `KNX_Temperature`, `KNX_Analog`) and
  `KNX_IPRouter` have not yet passed their tests against other KNX devices.
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
- **GPT-partitioned SD cards on a real board.** Supported in the file system
  configuration and tested on the host only.
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

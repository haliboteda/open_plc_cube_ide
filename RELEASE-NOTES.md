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

### Upgrade rules

> **The bootloader and the application must be upgraded together. This is not a
> recommendation.**

Application metadata (size, version, hash) lives in the bootloader's journal,
and 0.1.3 changed the journal format. A 0.1.3 bootloader cannot read a journal
written by 0.1.2:

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

> ⚠️ **The device side is complete and tested; the operator-facing tooling is
> not.** `takeown` and `setowner` are bootloader commands, and today they are
> driven only by internal test scripts — `IAPTool` does not expose them yet.
> Until it does, claiming a board is not a supported customer operation.

### Known issues

- **The application's `[BOOT] millis=` banner reaches the RS232 terminals only
  by accident of timing.** The core prints it before anything drives the
  transceiver enable high, so the MAX3221 is nominally shut down — but its
  charge pump still holds enough residual voltage to produce valid RS-232 levels
  for a few milliseconds, and the banner slips out on that. Measured at
  `millis=12`. **Nothing guarantees that window.** An application that starts
  more slowly will lose the line entirely, so do not build anything on it and do
  not add further start-up printing at that point in the core.
- **A stray byte appears on the serial console immediately before `[BOOT]`.**
  Cause identified: when the bootloader disables the transceiver on its way out,
  the charge pump collapses and the line slides from mark toward 0 V, which the
  receiver reads as a start bit. That is the inherent cost of handing the
  application a board whose transceiver is in a known-off state, which is
  deliberate. Display only, with no functional effect.
- **Discovery rate limiting is a fixed window, not a token bucket.** Nominally
  50 replies/sec; a burst straddling a window boundary has been measured at 60.
  Treat 50 as approximate, not as a guarantee.
### Not verified

- **MAC addresses are unique across boards.** Derivation from the chip UID
  replaced the hardcoded `00:80:E1:00:43:21`, and one board was confirmed to
  derive and use its own address consistently in both the bootloader and the
  application. Only one board was available, so uniqueness *between* boards has
  never been observed. Put it on the production checklist.
- **Behaviour when power is lost mid-upgrade.** Staging changed what the risky
  window is: losing power during the transfer should now be harmless, and only
  the few seconds spent erasing and writing can leave the application region
  half-written. Neither half has been reproduced by *removing power*.

  The erase/write window has, however, been hit by accident with a reset, and it
  behaved as predicted: the application became invalid, the board reported
  `metadata present` with `App signature invalid or absent`, and re-uploading
  restored it. **That window is also wider and easier to hit than assumed** —
  the upload tool exits as soon as it has sent the last byte, while the board is
  still verifying, erasing and copying out of SDRAM. Anything automating an
  upgrade should wait for the board's own `Checksum and signature OK` rather
  than for the tool to exit.
- Long-term stability under a real OpenPLC runtime.

### Before shipping

The signing key and the IAP password in this repository are placeholders, and
the placeholder private key is committed — anyone with the source can sign an
image the board accepts. Run `IAPServer/keys/rotate_keys.sh`, then keep
`fw_signing_key.pem` off this machine. See `IAPServer/keys/README.md`.

---

## Release checklist

Everything here has burned someone at least once.

The first three are now checked by `TestTool/tools/selfcheck.ps1`, which also
runs the host-side tests. Run it before working through the rest by hand.

- [ ] `OPENPLC_FW_VERSION` in `Core/Inc/IAP_config.h` matches
      `OPEN-PLC.build.fw_version` in the core package's `boards.txt`. Nothing
      enforces this — the two live in different repositories with no shared
      build. → `tools/check-version-sync.ps1`
- [ ] Every mirrored file is in sync across the three repositories (see
      `docs/ARCHITECTURE.md`, "跨仓镜像的代码"). A divergence does not fail the
      build; it shows up at runtime as something unrelated.
      → `tools/check-mirror-sync.ps1`
- [ ] Everything verified in the live Arduino15 package has been copied back
      into the core package's git repository and committed.
      → `tools/check-core-sync.ps1`
- [ ] Secrets rotated, and the private signing key stored off-machine.
- [ ] Bootloader flashed over ST-Link/DFU and the application uploaded over
      IAP, in that order, on a board that previously ran the older release.

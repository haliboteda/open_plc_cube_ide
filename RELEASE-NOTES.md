# Release notes

Covers the whole product, not just this repository: the bootloader lives here,
the Arduino core package and `IAPTool` ship from their own repositories, and a
release is only meaningful as a matching set of all three.

Read [Upgrade rules](#upgrade-rules) before flashing anything onto a board that
already has firmware on it.

---

## Unreleased — 0.1.3

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

### Known issues

- **The bootloader reports version `0.1.2`.** `OPENPLC_FW_VERSION` in
  `Core/Inc/IAP_config.h` was not bumped, so the identity string and the `info`
  command both understate it. Cosmetic, but it makes tool logs show two
  different version numbers for one board and will send anyone debugging in the
  wrong direction.
- **The application's `[BOOT] millis=` banner never reaches the RS232
  terminals.** The core prints it before anything drives the transceiver enable
  high, and the MAX3221's charge pump is stopped until a sketch does that in
  `setup()` — the line is emitted into a transmitter that physically cannot
  produce RS-232 levels. Diagnostics only; nothing else depends on it.
- **A stray byte appears on the serial console around the bootloader-to-
  application handover.** Three candidate sources, none confirmed on hardware:
  the transceiver input floating after `HAL_UART_DeInit()` while the chip is
  still enabled, the charge pump collapsing when the bootloader disables it, and
  the charge pump ramping when the sketch enables it again. Display only.
- **Discovery rate limiting is a fixed window, not a token bucket.** Nominally
  50 replies/sec; a burst straddling a window boundary has been measured at 60.
  Treat 50 as approximate, not as a guarantee.
- **`iap_auth_report_backup_domain()` can report a false loss** of the backup
  domain. The witness register reads back a wrong value; the replay counter
  itself has been observed surviving a real power cycle. The report was made
  non-alarming in 2026-08-15; the root cause is still open.

### Not verified

- **MAC addresses are unique across boards.** Derivation from the chip UID
  replaced the hardcoded `00:80:E1:00:43:21`, and one board was confirmed to
  derive and use its own address consistently in both the bootloader and the
  application. Only one board was available, so uniqueness *between* boards has
  never been observed. Put it on the production checklist.
- Long-term stability under a real OpenPLC runtime.

### Before shipping

The signing key and the IAP password in this repository are placeholders, and
the placeholder private key is committed — anyone with the source can sign an
image the board accepts. Run `IAPServer/keys/rotate_keys.sh`, then keep
`fw_signing_key.pem` off this machine. See `IAPServer/keys/README.md`.

---

## Release checklist

Everything here has burned someone at least once.

- [ ] `OPENPLC_FW_VERSION` in `Core/Inc/IAP_config.h` matches
      `OPEN-PLC.build.fw_version` in the core package's `boards.txt`. Nothing
      enforces this — the two live in different repositories with no shared
      build.
- [ ] Every mirrored file is in sync across the three repositories (see
      `docs/ARCHITECTURE.md`, "跨仓镜像的代码"). A divergence does not fail the
      build; it shows up at runtime as something unrelated.
- [ ] Everything verified in the live Arduino15 package has been copied back
      into the core package's git repository and committed.
- [ ] Secrets rotated, and the private signing key stored off-machine.
- [ ] Bootloader flashed over ST-Link/DFU and the application uploaded over
      IAP, in that order, on a board that previously ran the older release.

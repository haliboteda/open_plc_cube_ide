#!/usr/bin/env python3
"""Put the linker-script option in .cproject back to ${PLC_LD_SCRIPT}.

CubeMX writes a hardcoded STM32H743IIKX_FLASH.ld into that option on every
generation, which silently pins the port-tool build to the bootloader's 120K
script. Run after generation; see $PROD/docs/build/CUBEMX-RULES.md.
"""

import re
import sys
from pathlib import Path

WANT = "${workspace_loc:/${ProjName}/${PLC_LD_SCRIPT}}"

# Only the linker option carries this id. ST's own "||"-separated record string
# also names the script, but that one is left alone on purpose -- rewriting it
# was tried on 2026-09-10 and changed nothing.
OPTION = re.compile(r'(linker\.option\.script[^\n]*?value=")([^"]*)(")')


def main():
    cproject = Path(__file__).resolve().parent.parent / ".cproject"
    if not cproject.is_file():
        print("restore_ld_script: %s not found" % cproject, file=sys.stderr)
        return 1

    # newline="" keeps the file's CRLF line endings byte for byte. Path.read_text
    # only grew that argument in 3.13, so go through open().
    with open(cproject, "r", encoding="utf-8", newline="") as fh:
        text = fh.read()

    hits = OPTION.findall(text)
    if len(hits) != 1:
        print("restore_ld_script: expected 1 linker script option, found %d - "
              "check .cproject by hand" % len(hits), file=sys.stderr)
        return 1

    if hits[0][1] == WANT:
        print("restore_ld_script: already %s, nothing to do" % WANT)
        return 0

    fixed = OPTION.sub(lambda m: m.group(1) + WANT + m.group(3), text, count=1)
    with open(cproject, "w", encoding="utf-8", newline="") as fh:
        fh.write(fixed)
    print("restore_ld_script: was %s, restored to %s" % (hits[0][1], WANT))
    return 0


if __name__ == "__main__":
    sys.exit(main())

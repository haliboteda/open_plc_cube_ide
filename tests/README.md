# Bootloader host tests

The real `IAPServer/*.c` compiled with the PC's own compiler against stubs,
plus two source checks. A standalone CMake project: configure this directory,
not the repository root (the root `CMakeLists.txt` forces `arm-none-eabi-gcc`).

Needs a `gcc` or `clang` that supports `-std=c11`, CMake 3.20+, Ninja, and Python 3 for the two
source checks (without Python they are skipped with a warning).

```
cd tests
cmake --preset host          # if gcc, cmake and ninja are on PATH
cmake --build --preset host
ctest --preset host
```

When the toolchain is not on PATH, put a `CMakeUserPresets.json` next to this
file (it is gitignored) that inherits `host` and names the tools, for example:

```json
{
  "version": 6,
  "configurePresets": [{
    "name": "local", "inherits": "host", "binaryDir": "${sourceDir}/build/host",
    "environment": { "PATH": "D:/Soft/mingw64/bin;$penv{PATH}" },
    "cacheVariables": {
      "CMAKE_C_COMPILER": "D:/Soft/mingw64/bin/gcc.exe",
      "CMAKE_MAKE_PROGRAM": "D:/Soft/mingw64/bin/ninja.exe"
    }
  }],
  "buildPresets": [{ "name": "local", "configurePreset": "local" }],
  "testPresets": [{
    "name": "local", "inherits": "host", "configurePreset": "local",
    "environment": { "PATH": "D:/Soft/mingw64/bin;$penv{PATH}" }
  }]
}
```

then use `--preset local`. The `PATH` entry lets the test binaries find the
compiler's runtime libraries.

## Tests

| ctest name | Case | Source |
|---|---|---|
| `T1-16` | certificate and auth core | `bootloader_unit/` |
| `T2-22-T2-23` | revocation area warns at 8 slots left, refuses the 97th | `owner_capacity/` phase `capacity` |
| `T1-33` | sector-15 compaction keeps what is in force | phases `compact`, `compact-verify` |
| `T2-24` | `setowner --wipe` reclaims every slot | phases `wipe`, `wipe-verify` |
| `T2-27` | a revocation naming the root in force is ignored | phase `self-revoke` |
| `T2-31` | a board with no root trusts nothing | phase `no-root` |
| `T2-32` | a factory reset returns the board to no root | phase `reset` |
| `T2-33` | 40 handovers in a row | phase `rotate` |
| `T2-34.*` | a sector-15 reclaim cut by a power loss, 24 scenarios | `sector15_reclaim/` |
| `P16` | the I-cache and the flash lock are restored on every exit | `checks/check_icache_is_restored.py` |
| `P17` | the `.cproject` linker script is still `${PLC_LD_SCRIPT}` | `checks/check_cproject_ld.py` |

Criteria: `OpenPLC_Docs/docs/engineering/HOW-TO-RUN-TESTS.md`. Each multi-step
entry runs its steps as separate processes, in order, in its own working
directory under the build tree (`run_steps.cmake`), so `ctest -j` is safe.

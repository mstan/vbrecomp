# BRINGUP.md â€” vbrecomp first-time setup

## Prerequisites

- **Python 3.10+**. Python 3.10 installs the declared `tomli` dependency;
  Python 3.11+ uses `tomllib` from the standard library.
- **CMake 3.20+**
- **C/C++ compiler.** On Windows: MSVC (Visual Studio 2022) OR MinGW-w64
  via MSYS2 (`mingw64/bin` on PATH). Both supported. The runtime is
  C99 + C++17.
- **SDL2 development package** (for the live runtime window and audio). A
  headless TCP-only `vb-runtime` build still configures when SDL2 is absent.
  Where to put it:
  - MSVC: `sdl2-msvc/SDL2-X.Y.Z/` next to the project (download
    `SDL2-devel-X.Y.Z-VC.zip` from libsdl.org)
  - MinGW: install via `pacman -S mingw-w64-x86_64-SDL2` or pkg-config
- **Git**, **bash** (MSYS2 ships it on Windows)

## First build

```bash
cd F:/Projects/virtualboyrecomp/vbrecomp

# Python recompiler tests
python -m unittest discover recompiler/tests

# Decoder coverage report (when a ROM is available)
python -m recompiler.cli.vbrecomp_coverage --rom roms/homebrew_test.vb

# Runtime
cmake -S . -B build -G "MinGW Makefiles"   # or "Visual Studio 17 2022"
cmake --build build --target vb-runtime

# Smoke
./build/runtime/vb-runtime.exe --port 4390 &
python tools/_ping.py --port 4390
```

Without `-DVBRECOMP_GAME=<module>`, `vb-runtime` uses the generic interpreter
host. Pass `--rom <cartridge>` to run it with TCP control. Pass `-DVBRECOMP_GAME=<module>` after generating
`generated/<module>_full.c`, `_dispatch.c`, and `.h` to run recompiled code.

## Acquiring the Beetle VB oracle

From PowerShell, run the pinned preparation recipe from this framework:

```powershell
.\tools\prepare-oracle.ps1 -Destination ..\beetle-vb
```

The destination must be new. The script clones the pinned independent Beetle
revision, applies observation-only diagnostics from `tools/oracle-diagnostics.patch`,
and builds its static library with native MinGW executables. Override
`-Toolchain` and `-Git` for non-default installations. Existing checkouts are
preserved. Configure with `-DBEETLE_VB_ROOT=<absolute oracle directory>` and
build `vb-beetle`. See [PARITY.md](PARITY.md) for the state schema and comparison
commands. The core is optional and is not linked into the player executable.

## Acquiring a ROM

vbrecomp ships no ROM data. Place any of:

- A small homebrew test ROM (e.g. one of the vbjaengine demos, a
  `vbtests.vb` from a homebrew dev kit)
- A commercial cartridge dump owned legally by the operator

into `roms/` and reference it from a TOML in `games/` (real configs
live in the per-game sibling repos).

## Ghidra V810/V830 support (Phase 2+)

See `ghidra/README.md` for the third-party SLEIGH module link and
installation steps. Ghidra is reference-only: labels, xrefs, block
discovery. **Never** trusted for execution semantics â€” that's the
oracle's job.

## Troubleshooting

- **"python: command not found"** â€” ensure Python 3.10+ is on PATH.
  `py -3.11 -m unittest discover recompiler/tests` is a fallback on Windows.
- **`tomllib` ImportError** â€” on Python 3.10, install the declared project
  dependencies so the `tomli` compatibility package is available.
- **CMake can't find SDL2** â€” the runtime falls back to a TCP-only build.
  Install SDL2 when a live window or audio output is needed.
- **`vb_stub_abort` fired on first run** â€” that's the system working
  as designed. Read the banner, find the named subsystem (e.g.
  `"unmapped read at 0x02000028"`), and either:
  - Add the missing handler in the relevant `runtime/src/*.c`, OR
  - Fix the recompiler to not emit code that touches the unmapped
    address.
- **TCP port already in use** â€” choose another via `--port N` or kill
  the lingering process.

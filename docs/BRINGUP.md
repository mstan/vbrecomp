# BRINGUP.md — vbrecomp first-time setup

## Prerequisites

- **Python 3.11+** (for `tomllib` in the stdlib)
- **CMake 3.20+**
- **C/C++ compiler.** On Windows: MSVC (Visual Studio 2022) OR MinGW-w64
  via MSYS2 (`mingw64/bin` on PATH). Both supported. The runtime is
  C99 + C++17.
- **SDL2 development package** (for Phase 4+ runtime; not required to
  build the Phase 1 skeleton). Where to put it:
  - MSVC: `sdl2-msvc/SDL2-X.Y.Z/` next to the project (download
    `SDL2-devel-X.Y.Z-VC.zip` from libsdl.org)
  - MinGW: install via `pacman -S mingw-w64-x86_64-SDL2` or pkg-config
- **Git**, **bash** (MSYS2 ships it on Windows)

## First build — Phase 1 skeleton

```bash
cd F:/Projects/virtualboyrecomp/virtualboyrecomp

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

The Phase 1 runtime does NOT link any generated C. It opens the TCP
server and idles. This is enough to validate the harness.

## Acquiring the Beetle VB oracle (Phase 2+)

From **PowerShell** (MSYS2 bash silently fails to spawn cc1.exe — see
auto-memory `reference_vbrecomp_build_via_powershell.md`):

```powershell
# One-time PATH prefix for the session
$env:PATH = "C:\msys64\mingw64\bin;$env:PATH"
Set-Location F:\Projects\virtualboyrecomp\virtualboyrecomp

# Clone (skip if beetle-vb/ already populated)
git clone https://github.com/libretro/beetle-vb-libretro.git beetle-vb
Set-Location beetle-vb

# Build the static archive. `platform=win` or `platform=mingw_x86_64`
# both work in MSYS2's mingw64 shell.
& "C:\msys64\mingw64\bin\mingw32-make.exe" platform=win STATIC_LINKING=1 -j8

# Result on Windows: beetle-vb/mednafen_vb_libretro.dll
#   — despite the `.dll` extension this is a real `ar rcs` archive,
#     produced by libretro's STATIC_LINKING=1 mode. Verify with
#     `file mednafen_vb_libretro.dll` → "current ar archive".
# On Unix the equivalent file is `mednafen_vb_libretro.so`.
```

The top-level CMake searches multiple candidates
(`libmednafen_vb.a`, `mednafen_vb_libretro.dll`, `mednafen_vb_libretro.so`)
and announces the one it found. The `vb-beetle` oracle target is built
automatically when any of them is present.

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
discovery. **Never** trusted for execution semantics — that's the
oracle's job.

## Troubleshooting

- **"python: command not found"** — ensure 3.11+ is on PATH. `py -3.11
  -m unittest discover recompiler/tests` is a fallback on Windows.
- **`tomllib` ImportError** — you're on 3.10 or older. Upgrade.
- **CMake can't find SDL2** — only an issue from Phase 4 onward. The
  skeleton target does not link SDL2.
- **`vb_stub_abort` fired on first run** — that's the system working
  as designed. Read the banner, find the named subsystem (e.g.
  `"unmapped read at 0x02000028"`), and either:
  - Add the missing handler in the relevant `runtime/src/*.c`, OR
  - Fix the recompiler to not emit code that touches the unmapped
    address.
- **TCP port already in use** — choose another via `--port N` or kill
  the lingering process.

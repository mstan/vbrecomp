# Assisted coverage and builds

Windows consumers keep a ROM-free `game-build.json` (module, full ROM SHA256,
UI option and optional CMake settings/extra targets). Their thin PowerShell
wrapper calls `tools/build_game.ps1` here. Use native Windows Python 3.11+ and
MinGW CMake/Ninja/GCC, independent of the caller's working directory:

```powershell
./tools/build.ps1 -Rom F:/private/game.vb -Build F:/build/game
./tools/build.ps1 -Rom F:/private/game.vb -Build F:/build/release -Production
```

The default includes the launcher and developer debugger. Production disables
the TCP debugger and CPU trace. `-NoUi` builds without the launcher;
`-FastBuild` is development-only where the consumer supports it. An independent
Beetle observation build requires explicit `-Oracle` with a freshly prepared
checkout; an unrelated local archive is never selected implicitly. `-Python`,
`-Framework`, `-Ui`, `-Toolchain`, `-Jobs` and `-SkipRegenerate` are available.
The helper checks cartridge identity, stages SDL2, builds extra game targets,
and runs CTest. No ROM is distributed.

Developer protocol additions:

* `set_gpr index=1..31 val=0..4294967295` changes one paused architectural
  register. Register zero is immutable.
* `pc_set pc=...` changes the paused architectural PC, requiring an even
  unsigned 32-bit address. Resuming uses that PC, including after frame yields.
* `fallback_coverage` reports a bounded, lifetime histogram of actual hybrid
  fallback PCs and hit counts. Capacity is 4096; `overflow_hits` explicitly
  counts unrecorded hits when full. Pure interpreter execution is not fallback.
  The counters remain enabled in production, while the TCP interface does not.

`tools/test_debug_writes.py --runtime EXE --rom PRIVATE_ROM` validates register
writes, rejected values and actual changed-PC instruction execution in hybrid
and interpreter modes. Run unattended tests with `SDL_AUDIODRIVER=dummy` and
`SDL_VIDEODRIVER=dummy`; keep normal audio in player builds. The interpreter
CTest also fills the histogram and verifies its overflow accounting.

For assisted surveys, stop at a verified original game controller boundary and
invoke original loaders with their real register/display contracts. Do not count
black screens, menu-only routes or uninitialized state as room coverage. Verify
actual selectors and visible screenshots as well as execution counters. Seed
only original cartridge code proven by execution and ROM references. Compare
native and interpreter CPU, memory, devices, both eyes and audio; use an
independent oracle when available. Presented color/widescreen surfaces need
their own visual review: raw guest parity alone does not validate enhancements.

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

Generated C now defaults to whole-function partitions of about 8192 original
instructions. `--unit-instructions 0` retains a single body source. An explicit
CMake manifest selects only current parts, even when older generated files
remain. Function bodies, exact-PC dispatch, instruction count and header stay
identical across partition sizes; the compiler can build independent units in
parallel instead of optimizing one enormous translation unit.

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

`tools/run_assisted_suite.py --runtime EXE --rom PRIVATE_ROM --suite SUITE.json
--out NEW_DIRECTORY` executes ROM-bound game route files in separate hybrid
and interpreter processes. Repeat `--mod PACKAGE.vbmod` to enable optional
native presentation (aspect options use 32:9); the control remains stock.
`--case ID` selects a subset. It records exact input identities, state hashes,
screenshots, selector checks, nine-plane comparisons and bounded fallback maps.
The default requires zero fallback; `--allow-fallback` is for baseline harvests.
WRAM writes, paused GPR/PC assists and original controller anchors are explicit
route actions. A comparison pass certifies those sampled states, not every
campaign branch or unassisted playthrough.

Games may explicitly compile `VB_BRANCH_OVERRIDES=1` for guarded, game-owned
simulation extensions. Other consumers compile each original branch expression
directly. Rules require an exact cartridge PC and original conditional-branch
word, a signed register comparison and a fixed bound. They preserve the PSW
and original taken/not-taken cycle charges; unmatched instructions retain their
original behavior. Registration is bounded to 128 entries, rejects duplicates
and unconditional/NOP encodings, and resets restore stock behavior. The
`branch_rules` debugger report lists bounds, hits and changed decisions.

This changes the selected game's simulation and is distinct from presentation
replay. A fixed rule profile must be part of any future multiplayer
compatibility identity; these ports do not qualify cross-build netplay from
this test. Wario's fixed aspect profiles and adaptive maximum are selected at
startup, independent of later host-window resizing. Compare both processes
with `--matched-mods` for such a profile; retain separate stock controls.

`VbViewportLayout.unwrapped_object_coordinates` is a separate presentation
extension for games proven to retain full signed JPX values. Each eye adds
signed parallax before clipping, avoiding ten-bit wrap in wide margins. The
default and native-width replay retain the original hardware behavior. Synthetic
VIP tests exercise x=700, the x=510 parallax seam, negative x, both eyes and
native replay parity on Windows and Linux.

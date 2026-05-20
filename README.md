# vbrecomp

Static V810 (Nintendo Virtual Boy) → C recompiler + runtime + Beetle VB
oracle harness.

V810 cartridge ROM → C → native executable.

This repo holds the **framework**: the Python recompiler, the C/C++
runtime skeleton, the TCP debug server, the always-on ring buffers
(wtrace / fntrace / frame ring), the VIP / VSU / IRQ / timer hardware
simulation, and the Beetle VB libretro-driven oracle binary. It does
NOT contain any game ROM or game-specific generated C.

To build a runnable cart, this framework is consumed by a per-game
"recomp" repo — e.g. [`MarioTennisVirtualBoyRecomp`](https://github.com/mstan/MarioTennisVirtualBoyRecomp).
The per-game repo lays this repo down as a `vbrecomp/` subdirectory,
drops the cart ROM under its own `roms/`, runs the recompiler to emit
`generated/<game>_*.c`, and links it into vb-runtime.

## Repo layout

```
recompiler/          Python V810→C recompiler tool
runtime/             C/C++ runtime + TCP debug server + ring buffers
                     (wtrace, fntrace, frame snapshots)
tools/               Python probes that talk to the runtime over TCP
                     (_ping, _wtrace_summary, _wram_diff, _fntrace_walk,
                      _worlds_decode, _framebuf_diff, _vip_diff, …)
docs/                Architecture, hardware notes, instruction status
games/               Per-game TOML configs (example files committed;
                     each real game also lives in its own per-game repo)
```

External directories (NOT in this repo — provided by the consuming
per-game repo, all gitignored at that level):

```
roms/                The cart ROM. © the cart publisher; never committed.
generated/           Recompiler output. Derivative of the cart ROM —
                     regenerate locally, never commit.
beetle-vb/           Clean upstream clone of mednafen-vb-libretro.
                     Local changes (if any) live as project-side .patch
                     files applied at cmake-configure time, NEVER as
                     commits inside this submodule.
build/               CMake build dir.
```

## Build (standalone, no game)

```bash
python -m unittest discover recompiler/tests
cmake -S . -B build
cmake --build build --target vb-runtime
./build/runtime/vb-runtime.exe --port 4390
python tools/_ping.py --port 4390
```

This produces a `vb-runtime` linked against `no_game_linked.c` — the
runtime starts, the TCP debug server responds, but no cart code is
present. Use this to develop the runtime / recompiler in isolation.

## Build with a game

See the consuming per-game repo's README, e.g.
[`MarioTennisVirtualBoyRecomp`](https://github.com/mstan/MarioTennisVirtualBoyRecomp).

## Read these first

- [CLAUDE.md](CLAUDE.md) — project constitution. Read at session start.
- [PRINCIPLES.md](PRINCIPLES.md) — system-agnostic recomp rules.
- [DEBUG.md](DEBUG.md) — debug loop contract.
- [TCP.md](TCP.md) — debug server protocol (wtrace + fntrace included).
- [PLAN.md](PLAN.md) — phased milestones (P1 ✓ … P4 ✓ for Mario's Tennis).
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — how the pieces fit.
- [docs/HARDWARE_NOTES.md](docs/HARDWARE_NOTES.md) — V810 / VB facts.
- [docs/INSTRUCTION_STATUS.md](docs/INSTRUCTION_STATUS.md) — opcode coverage.

## Sibling projects

vbrecomp shares conventions with [`psxrecomp`](https://github.com/mstan/psxrecomp),
[`snesrecomp`](https://github.com/mstan/snesrecomp),
[`nesrecomp`](https://github.com/mstan/nesrecomp), and
[`segagenesisrecomp`](https://github.com/mstan/segagenesisrecomp). See
[LICENSE](LICENSE) for attribution.

## Licence

MIT (see [LICENSE](LICENSE)). The cart ROMs and any data derived from
them remain © their original publishers and are never included in
this repo or any of its per-game consumers' histories.

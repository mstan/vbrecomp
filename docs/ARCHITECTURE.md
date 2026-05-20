# ARCHITECTURE.md — vbrecomp

## Birds-eye view

```
                                    ┌───────────────────┐
                                    │  V810 Manual +    │
                                    │  Ghidra labels +  │
                                    │  game TOML        │
                                    └─────────┬─────────┘
                                              │
                                              ▼
   ROM bytes ──► [Python recompiler] ──► generated/*.c ──► [Runtime CMake build]
                       │                                            │
                       │                                            ▼
                       │                                     vb-runtime.exe  (port 4390)
                       │
                       │  decoder report + INSTRUCTION_STATUS.md
                       ▼

                                                  vb-beetle.exe  (port 4391)
                                                  Beetle VB libretro core
                                                  (oracle)


   Diagnostic tools ──TCP──► both processes ──► find first divergence
```

## Two binaries, same wire protocol

Per `CLAUDE.md` Rule 14: `vb-runtime.exe` and `vb-beetle.exe` are
**separate processes**. Each owns its own SDL window, its own input
state, and its own TCP debug server. A diagnostic tool written against
either backend works against the other by changing the port. They
never share memory and they are never stepped in lockstep.

## Recompiler pipeline (Python)

`recompiler/cli/vbrecomp_main.py`:

```
load_game_config(path)        # recompiler/config/game_toml.py
  → ROM bytes + entry_pc + load_base
load_vb_header(rom_bytes)     # recompiler/rom/header.py
  → game title, sha256, sizes
build_memory_map(rom, cfg)    # recompiler/rom/memory_map.py
  → MemoryMap (region table)
scan_decode(rom, entry)       # recompiler/v810/decoder.py
  → list of DecodedInstruction
discover_functions(decoded)   # recompiler/v810/analysis.py  [P2]
  → list of FunctionRange
build_cfg(fn, decoded)        # recompiler/v810/analysis.py  [P2]
  → ControlFlowGraph
lift(insn → IR)               # recompiler/v810/lifter.py    [P3]
  → list of IR ops per block
emit_c(ir, fn)                # recompiler/v810/emitter.py   [P3]
  → C source string
```

P1 implements only the first three lines plus a partial decoder. The
later stages exist as files with `NotImplementedError` markers and a
`PHASE = "..."` constant.

## Runtime (C/C++)

`runtime/src/main.cpp` is the entry point. Flow:

1. Parse args (`--config X.toml`, `--rom Y.vb`, `--port N`)
2. Load game TOML (host-side, before any guest execution)
3. Allocate `CPUState` and wire bus function pointers
4. `memory_init(rom_path)` — map ROM, allocate WRAM, register MMIO
   handlers
5. `debug_server_start(port)` — non-blocking TCP listen
6. `ring_frame_init()` — allocate the 36k frame ring
7. `freeze_heartbeat_start()` — watchdog thread
8. SDL window, main loop:
   - Poll SDL events (window close, keys, pad mapping)
   - `debug_server_poll()` — service one command per call
   - `vb_dispatch(cpu, RESET_VECTOR)` once at boot; thereafter
     the recompiled code returns naturally to `main` per its own
     halt model. On a "no game linked" build the main loop simply
     services TCP commands forever — fine for `ping` validation.
   - `ring_frame_record(cpu, /*hardware state*/)`
   - SDL_RenderPresent at 50.27 Hz pacing

Every unmapped read/write, every unrecognised MMIO register, every
"shouldn't be reachable" branch in C calls `vb_stub_abort(...)`.

## Always-on ring buffers

| Ring                  | Size            | Records                                        |
|-----------------------|-----------------|------------------------------------------------|
| `frame`               | 36 000          | full CPU + VIP + VSU + pad snapshot per frame  |
| `fntrace`             | 1 048 576       | every `vb_dispatch` call (frame, target, lp, args) |
| `wtrace` (per range)  | 1 048 576 each  | every store inside an armed `(lo,hi)` window   |
| `crash`               | 65 536          | every dispatch addr + every exception entry    |
| `freeze_heartbeat`    | 64              | one snapshot every 100 ms (~6.4 s window)      |

All rings are sized at boot and never reallocated. Eviction is
modular-index (`write_idx % CAP`). A 64-bit `seq` counter is preserved
across eviction so absolute ordering is reconstructable.

## Stub abort = the only failure floor

A stub is fatal. `vb_stub_abort()` (in `runtime/src/stub_abort.c`):

1. Writes a banner to stderr (the one whitelisted printf in the
   project — see `CLAUDE.md` Rule 3 exception).
2. Calls `crash_trace_dump_json("vb_last_run_report.json")` to flush
   every ring buffer to disk.
3. Calls `abort()`.

No code path returns from `vb_stub_abort()`. There is no "soft" mode
in committed builds.

## Where ground truth lives

- **CPU semantics:** V810 manual + Beetle VB oracle.
- **MMIO semantics:** Virtual Boy Programmer's Manual + oracle.
- **Game-specific quirks:** the per-game TOML in the game's sibling
  repo (e.g. `F:/Projects/virtualboyrecomp/MariosTennisRecomp/game.toml`).
- **Generated code:** the recompiler, never hand-edited.

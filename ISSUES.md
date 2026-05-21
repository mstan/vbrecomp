# ISSUES.md

Known gaps and deferred work. Companion to `STUBS_TO_FIX.md` — the
stubs file tracks `vb_stub_abort()` callsites; this file tracks
larger pieces of work that have been *consciously deferred* and the
reason. Mario's Tennis ships without these; the next commercial cart
may demand any subset of them.

## Cycle accounting — per-opcode table (deferred)

`runtime/src/main.cpp` advances device emulation with a constant
`CYCLES_PER_BB = 3`. Real V810 instruction timing varies from 1
cycle (most ALU ops, register MOV) to 38+ cycles (DIVU) and 43 for
DIVF.S. The current approximation is good enough that Mario's
Tennis plays at roughly real-time speed against the VIP frame
clock; frame-perfect timing parity with Beetle would need a per-
opcode table.

**Source of truth:** Beetle's `v810_op_table_msvc.inc` and the
`timestamp +=` increments scattered through
`beetle-vb/mednafen/hw_cpu/v810/v810_cpu.cpp::fpu_subop`,
`Step_RB_DEBUG()`, and the per-format dispatcher.

**Estimated work:** ~200 lines: a `uint8_t v810_cycles[64][/*subop*/]`
table indexed by Format + opcode, threaded through `emit_function`
so each emitted instruction increments a per-call cycle counter
that main.cpp consumes instead of the BB count. Decision pending
the first cart that visibly drifts.

## V810 FP exception machinery (deferred)

`recompiler/v810/emitter.py::_emit_format_vii` implements ADDF.S,
SUBF.S, MULF.S, DIVF.S, CMPF.S, CVT.WS, CVT.SW, TRNC.SW with
correct PSW Z/S/CY/OV semantics but does NOT model the V810's
FRO / FIV / FZD / FOV / FUD / FPR exception flags or their
INVALID_OP_HANDLER_ADDR / FPU_HANDLER_ADDR delivery paths. Beetle
implements these in
`beetle-vb/mednafen/hw_cpu/v810/v810_cpu.cpp::CheckFPInputException`
and `FPU_DoException` (lines 837-902).

In practice this means a cart that feeds a subnormal / NaN /
infinity to an FP op will silently get a host-float result rather
than the V810 exception. Mario's Tennis never does this. Any cart
that relies on FP exceptions for error handling (none known) would
need this implemented.

**Estimated work:** ~50 lines per FPP op, plus an FPU_HANDLER_ADDR
exception delivery path mirroring `interrupts.c::vb_irq_force_handler`.

## CAXI — compare-and-exchange (stubbed)

`recompiler/v810/emitter.py::_emit_format_vi` aborts on opcode 0x3A.
The V810 hardware does word-aligned `if (*addr == r2) { *addr = r1; }`
atomically; without an OS-level mutex implementation in the cart this
is just a bus operation. No commercial VB cart we have access to
uses CAXI. Re-evaluate when one appears.

**Estimated work:** ~15 lines (read/compare/write through `cpu->write32`)
plus correct PSW flag updates per the V810 manual.

## BSU — bit-string operations (stubbed)

All 12 BSU sub-ops in `recompiler/v810/emitter.py::_emit_format_vii`
opcode 0x1F abort. The BSU is a complex multi-instruction-step
state machine (each BSU op runs for many cycles, can be interrupted,
resumes from saved state in r26-r30). Beetle implements the full
state machine in `beetle-vb/mednafen/hw_cpu/v810/v810_cpu.cpp`
search-bstr / move-bstr / arithmetic-bstr handlers.

Mario's Tennis uses zero BSU ops (verified by walking 27,980
instructions). The next commercial cart that ships a fast `memcpy`
or `memset` (very common in BSU code) will need this. Estimate: an
order of magnitude more work than CAXI — the resumable-after-IRQ
behaviour is non-trivial.

## Cart-RAM (region 6) reads/writes (aborts)

`runtime/src/memory.c` aborts on every access to region 6
(0x06000000-0x06FFFFFF). This region is the cart's optional SRAM /
EEPROM for save data. Mario's Tennis has no save data and never
touches it. Any cart with save support (Jack Bros., Galactic
Pinball-with-options, Mario Clash with save data, custom homebrew)
needs:

* Backing storage sized by the cart header's RAM-size field.
* Optional persistence to a `.sav` file alongside the cart.

**Estimated work:** ~80 lines + save-file persistence in `main.cpp`.

## Cart-expansion (region 4) reads/writes (aborts)

Same shape as region 6; region 4 is reserved for cart-attached
expansion hardware (none ever shipped commercially). Should
remain a hard abort.

## Reserved region 3 (correctly aborts)

Region 3 is V810-reserved; any cart accessing it is buggy. The
abort here is correct behavior, not a deferral. Listed for
completeness.

## Input serial-shift IRQ (deferred behind InstantReadHack)

`runtime/src/input.c` returns the pad word directly from
SDR_LO/SDR_HI without modeling the 640-cycle-per-bit serial shift
or the K_INPUT IRQ that fires on completion. This matches Beetle's
`InstantReadHack = true` default. Carts that explicitly wait for
the K_INPUT IRQ rather than polling SDR would observe no IRQ. No
cart we have access to relies on this — most poll SDR every frame
in VBlank.

**Estimated work:** ~60 lines: a timer-driven shift counter in
input.c that raises K_INPUT through `vb_irq_assert(VBIRQ_SOURCE_KEY)`
640 cycles after each SCR write with the start-bit set.

## Mario's Tennis: marios_tennis.toml hand-curated function seeds

`marios_tennis.toml` currently lists 5 explicit `[functions].seeds`
entries (the score-state-machine table at ROM offset 0x160C8 that
the static walker can't follow). These were added after the first
score-against-player crash. Other indirect-dispatch tables exist
in the cart (we found 211 candidate cart-PC-range pointers via the
table-shape heuristic) but were not added because the bulk-add
introduced false-positive functions that broke boot.

Tightening the pointer-scan heuristic to net-add only true entries
is tracked separately and would let the toml be reduced toward
empty.

## Ghost-decode warnings — vb_fn_FFFFFF[60-F0]

The cart's ISR vector slots at 0xFFFFFF60..0xFFFFFFF0 are padded
with 0xFFFF bytes. `analysis.py::HANDLER_VECTORS` walks them as
function entries; the decoder interprets `0xFFFF_FFFF` as
`ST.W r31, -1[r31]` and the walker emits 7 unused-label warnings
during build. The functions are never reached at runtime — the
real cart trampolines elsewhere — but the labels generate gcc
warnings each build.

Fix: skip handler vectors whose first instruction is the 0xFFFF /
0xFFFF padding pattern. Tracked in audit punch list, not blocking.

# ISSUES.md

Known gaps and deferred work. Companion to `STUBS_TO_FIX.md` — the
stubs file tracks `vb_stub_abort()` callsites; this file tracks
larger pieces of work that have been *consciously deferred* and the
reason. Mario's Tennis ships without these; the next commercial cart
may demand any subset of them.

## V810 floating-point fidelity — host float vs SoftFloat (deferred)

> **What to look for if this ever bites:** wrong/odd FP results, or a
> missing FP exception, in a cart that does real floating-point math
> (3D, physics). Search `recompiler/v810/emitter.py::_emit_format_vii`.

**Where it lives:** every V810 FP op — `CMPF.S`, `CVT.WS`, `CVT.SW`,
`TRNC.SW`, `ADDF.S`, `SUBF.S`, `MULF.S`, `DIVF.S` (Format VII / FPP,
primary opcode `0x3E`) — is emitted using the **host x86 `float`**, via
the `union { uint32_t u; float f; }` reinterpret idiom and native C float
arithmetic (`_a.f == _b.f`, `(float)_i`, `_a.f * _b.f`, …). The PSW
Z/S/CY/OV semantics ARE oracle-correct (mirror Beetle's
`SetFPUOPNonFPUFlags`); only the float *value* path can differ.

**Why it can diverge from the oracle / real hardware:** the V810 (and the
Beetle/Mednafen oracle) compute FP with an exact IEEE-754 single-precision
**SoftFloat** (`beetle-vb/mednafen/hw_cpu/v810/fpu-new/softfloat.*`). Host
`float` is *usually* bit-identical for normal finite inputs but can differ
on: NaN/Inf payload + sign propagation; subnormals (flush-to-zero);
rounding-mode / intermediate-precision edges (x87 80-bit vs SSE, FMA
contraction, last-bit rounding of borderline results). Two sub-gaps:

1. **Precision / rounding** — even finite inputs can yield a 1-ULP
   difference vs SoftFloat.
2. **FP exception machinery NOT modeled** — the FRO / FIV / FZD / FOV /
   FUD / FPR flags and their `INVALID_OP_HANDLER_ADDR` /
   `FPU_HANDLER_ADDR` delivery are absent (Beetle:
   `v810_cpu.cpp::CheckFPInputException` / `FPU_DoException`, lines
   837-902). A cart feeding a subnormal/NaN/Inf gets a silent host-float
   result instead of the V810 FP exception. NB: *unimplemented* FPP
   sub-ops abort via `vb_stub_abort`; the *implemented* ones above never
   abort — they silently use host float.

**Status for Mario's Tennis:** not a problem. The 1.4M-instruction cpuhook
match (Axis-1, `VB_ACCURACY_BURNDOWN.md`) would have caught any FP
divergence in the boot window — MT either doesn't exercise the divergent
edges or doesn't use FP there.

**How to fix (when a target needs it):** vendor a SoftFloat (reuse
mednafen's `fpu-new/softfloat`) and route every `_emit_format_vii` FP op
through it instead of host `float`; then add the FP-exception delivery
path (mirror `interrupts.c::vb_exception` with `FPU_HANDLER_ADDR`). Add
crafted-input regression cases to `tools/isa_semantics_check.py`
(compile-and-run the emitted C vs oracle-derived golden) — the same
harness that validated the MUL/DIV fixes.

**Estimated work:** SoftFloat vendor + wire-through ~150 lines; exception
delivery ~50 lines per op.

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

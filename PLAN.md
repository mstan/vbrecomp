# PLAN.md — vbrecomp Milestones

> Historical milestone plan. The per-instruction cycle model, event-driven
> timing, VIP renderer, and SDL runtime described here have since landed.
> Consult `VB_ACCURACY_BURNDOWN.md` for the maintained implementation status;
> retain this document for its original sequencing and design rationale.

The plan deliberately front-loads **visual ground truth** so you, the
user, get incremental visible progress instead of waiting until P5
for the first pixel. Every milestone below is *real* work — no
stubs, no synthetic test patterns. Earlier visibility comes from
building the oracle first (Beetle VB has been a complete emulator
for years; we wrap it) and from a bootstrap-only codegen pass that
lets the cart's own code program the VIP, even before the full game
is recompiled.

---

## Phase 1 — Skeleton (DONE)

**Goal:** Project compiles, tests pass, TCP `ping` responds.

✅ 47/47 Python tests pass.
✅ `vb-runtime.exe` builds via PowerShell-driven CMake + MinGW.
✅ `ping`, `frame`, `get_registers`, `read_ram`, `memory_map`, `quit`
   all respond.
✅ Mario's Tennis ROM loaded, first 16 bytes of cart read over TCP.
✅ Decoder: 96.74% coverage on Mario's Tennis after Sacred Tech Scroll
   opcode corrections (CLI 0x16, SEI 0x1E, CAXI Format VI 0x3A).

---

## Phase 2 — Decoder complete + L1 oracle parity (DONE)

**Goal:** Every V810 opcode in the Sacred Tech Scroll is decoded;
mnemonics match Beetle VB's authoritative op table on representative
ROMs.

✅ `isa.py` populated per the Sacred Tech Scroll. Every primary
   opcode + every documented FPP/BSU sub-op explicit; reserved slots
   marked `Invalid`. Doc regenerated mechanically from
   `recompiler/cli/vbrecomp_status.py`.
✅ `analysis.py` — `RomImage`, `trace_reset_trampoline`,
   `cfg_walk_from_seeds`, `discover_functions`, plus a jump-table
   resolver (`_try_resolve_jump_table` + `cfg_walk_with_table_resolution`)
   for the canonical `MOVHI/LD.W → JMP rX` dispatch pattern.
✅ beetle-vb cloned + static archive built (`mednafen_vb_libretro.dll`,
   which is `ar rcs`-formatted despite the suffix).
✅ `test_decoder_oracle.py` — parses Beetle's `v810_op_table_msvc.inc`
   as the static oracle; full per-instruction mnemonic parity verified.

**L1 oracle results (2026-05-20):**
| ROM             | Reachable instrs | Functions | Indirect | Unknowns | Mismatches |
| --------------- | ----------------:| ---------:| --------:| --------:| ----------:|
| Mario's Tennis  |           23,780 |       113 |        0 |        0 |          0 |
| V-Tetris        |           75,678 |       115 |        0 |        2 |          0 |

V-Tetris's 2 "unknowns" are both NEC-reserved primary opcodes (0x1B
and 0x36) reached via mis-resolved jump-table entries — both we and
Beetle classify them as INVALID, so they round-trip as oracle
matches. Below the 0.5% threshold by three orders of magnitude.

**P2 design notes (carry forward):**
- L1 oracle is *static* — it parses Beetle's compile-time dispatch
  table rather than running `vb-beetle.exe`. The SDL window + libretro
  driver port is deferred to P2.5 (it's a visual milestone, not a
  correctness gate). This kept P2 in pure-Python territory.
- Beetle's naming uses `op_EI`/`op_DI`; we follow Sacred Tech Scroll
  / NEC manual naming `CLI`/`SEI`. Same instructions; the oracle
  test bridges via an explicit alias map in `_beetle_op_table.py`.
- Jump-table resolver bounds reads by the ANDI mask preceding the
  dispatch; without an ANDI it defaults to 16 entries. Tightening
  this bound is the right move when a future cart shows >0.5%
  bogus walks.

---

## Phase 2.5 — Beetle VB oracle visible (≈ 1 session) — **FIRST VISUAL MILESTONE**

**Goal:** `vb-beetle.exe` runs Mario's Tennis on the libretro core,
opens its own SDL window, and exposes the JSON wire protocol on
port 4391. You see the actual game running in real-time before any
recompilation has happened.

**Steps:**
- Clone `beetle-vb-libretro`, build the static lib via PowerShell +
  mingw32-make (recipe in `docs/BRINGUP.md`).
- Port `psxrecomp/runtime/src/beetle_main.cpp` +
  `beetle_libretro.cpp` + `beetle_debug_server.c` patterns to VB:
  - `beetle_libretro.cpp` calls the libretro entry points and
    exposes the framebuffer + VRAM + VSU state via accessors.
  - `beetle_main.cpp` drives an SDL window at 50.27 Hz.
  - `beetle_debug_server.c` serves the *same* command names as
    `vb-runtime`, on port 4391.
- Extend the protocol with `vip_state`, `read_vram`, `read_charram`,
  `read_worlds`, `disasm` (used by the L1 oracle test).

**Exit criteria:**
- `vb-beetle.exe` opens an SDL window and renders Mario's Tennis
  title screen + gameplay correctly (Beetle has been doing this
  for years; if it doesn't, the libretro wiring is wrong).
- `python tools/_ping.py --port 4391` returns `ok:true`.
- `python tools/_screenshot.py --port 4391 --out beetle_title.png`
  produces a real PNG of the title screen.

This is the moment you can see Mario's Tennis with your eyes,
end-to-end, before any of our recompilation has produced a single
pixel. The oracle is now load-bearing for everything downstream.

---

## Phase 3 — Bootstrap codegen + execution (DONE)

**Goal:** The recompiler emits real C for a small bounded slice of
the cart — the reset trampoline plus the first ~50 functions
reachable from it. The runtime links the generated C and executes
it. No graphics yet, but the recompiled cart code is running, and
its writes to VIP/VSU/pad MMIO are reaching our (still log-only)
hardware shadows.

✅ `recompiler/v810/emitter.py` — direct DecodedInstruction → C
   emitter; ~1100 LOC; handles Format I/II/III/IV/V/VI in full,
   stub_aborts cleanly on FPP/BSU (P4+).
✅ `recompiler/cli/vbrecomp_codegen.py` — driver CLI; produces
   `generated/<module>_full.c` + `_dispatch.c` + `.h`.
✅ Mario's Tennis recompiled cleanly: **176 functions, 23,815
   instructions, ~3 MB of generated C, compiles in seconds** with
   gcc 15.2.0 (MinGW).
✅ Range-match dispatch + per-function leader-routing switch:
   yielded dispatch can resume at any basic-block leader, not just
   function heads. Lets the main loop interrupt mid-poll-loop and
   service TCP without losing PC.
✅ Step-budget yield (intra-function + inter-function): every basic
   block decrements `cpu->step_budget`; when it hits zero the
   recompiled code returns to `main.cpp` for a TCP poll, then
   resumes seamlessly. Bypasses native-stack overflow on tight
   intra-cart loops (WRAM-init: ~32k iterations, VIP-poll: infinite).
✅ Runtime VIP module extended (`runtime/src/vip.c`): full 512 KB
   shadow over the 0x00000000–0x0007FFFF window so cart-init writes
   to CHR RAM / BG maps succeed. Phase 4 lays the renderer on top of
   the same shadow.

**P3 boot evidence (2026-05-20, Mario's Tennis):**

```
vb-runtime: loaded ROM roms\marios_tennis.vb (524288 bytes), reset PC 0xFFFFFFF0
vb-runtime: listening on 127.0.0.1:4390
vb-runtime: dispatching to reset vector 0xFFFFFFF0
vb-runtime: first yield at pc=0xFFF81A9C (cart is running)
```

TCP probe of the live recompiled-cart state:

| Command | Result |
| ------- | ------ |
| `ping` | `{"ok":true,...}` |
| `get_registers` | `pc=0xFFF81AF2`, `r31=0xDEAD0001` (sentinel = top-level), `psw=0` |
| `read_ram 0x05000000 len=32` | `00 00 00 00 ... 00 F8 05 00 00 00 00 02 ...` — WRAM init writes |
| `read_ram 0x0005F800 len=16` | `00 00 00 00 1F E0 ...` — VIP control reg writes |

The cart has booted past trampoline → cart entry → 32k-iteration WRAM
clear → VIP CHR RAM init → into its VIP-frame poll loop. Without
P4's IRQ/timer driver the poll loop spins forever, which is correct
— it's waiting for VIP IRQs we haven't implemented.

**Carry-forward design notes:**
- Dispatch is range-match (each `vb_fn_<entry>` claims its byte
  range) and each function's body opens with a leader-routing switch.
  Costs one comparison per dispatch + one switch per function entry;
  enables any-leader resume.
- JMP r31 compiles to `cpu->pc = cpu->gpr[31]; return;`. The outer
  `vb_dispatch_call` exits when `cpu->pc == saved_lp`.
- Step budget decrements at every basic-block leader, NOT once per
  V810 instruction — coarse-grained but cheap, and sufficient to
  yield out of any loop.
- Generated C uses `vb_stub_abort_simple()` for every untranslated
  encoding; no silent stubs, no return-zeros.

---

## Phase 3 superseded section — original design

Original P3 plan is below for reference. The actual implementation
diverged on three points worth recording: (1) skipped the `ir.py`
SSA IR layer entirely (direct DecodedInstruction → C); (2) ran into
a stack-overflow loop and pivoted to loop-style trampoline dispatch;
(3) added range-match + leader-routing so yielded execution can
resume mid-function.

**Steps:**
- Implement `lifter.py` for every opcode used in the bootstrap (a
  small subset of the ISA — the bootstrap doesn't typically touch
  FP or bitstring).
- Implement `emitter.py` to produce `generated/<name>_full.c` +
  `generated/<name>_dispatch.c`.
- Drop `runtime/src/no_game_linked.c` from the build; replace with
  the real generated dispatch.
- Recompiled bootstrap runs to either:
  - the cart's first `HALT` (waiting for an interrupt), or
  - the cart's first `wait for VIP framebuffer` poll loop.
- TCP `get_registers` shows realistic register state after the
  bootstrap — sp pointing into WRAM, lp set, PSW reasonable.
- TCP `read_ram` from VIP register window shows the cart's
  initialisation writes (BG segment configuration, brightness
  setup).

**Exit criteria:**
- `INSTRUCTION_STATUS.md` shows `emitted:yes` for every opcode in
  the bootstrap.
- For Mario's Tennis: `vb-runtime` boots into the first VIP
  wait-loop without hitting `vb_stub_abort`.
- TCP `vip_state` shows the cart has written sensible initial
  values (DPCTRL set up, BRT* programmed, an INTENB mask).

---

## Phase 3.5 — VIP register state model + cross-process diff (≈ 1-2 sessions) — **SECOND VISUAL MILESTONE**

**Goal:** The VIP register model is *real* — not a shadow that swallows
writes silently, but a state machine that mirrors Beetle's behaviour
register-by-register. We verify by querying *both* `vb-runtime` and
`vb-beetle` over TCP at the same execution point and diffing.

**Steps:**
- Implement the real VIP state machine in `vip.c`:
  - XPSTTS / DPSTTS columnar progression with the 50.27 Hz cadence
  - INTPND / INTENB / INTCLR with bitwise semantics
  - BRTA / BRTB / BRTC / REST with the brightness pipeline shape
  - FRMCYC, DPCTRL / XPCTRL with the documented bit fields
  - LOCK / SYNCE — the gates the cart polls before drawing
- Add new TCP commands: `vip_state` returns every register + the
  full BG-segment + CHAR RAM + WORLDS + OBJ state, as JSON.
- Build `tools/_vip_diff.py`: runs the same bootstrap on both
  backends, dumps `vip_state` from each, diffs, reports the first
  divergent register.

**Exit criteria:**
- `tools/_vip_diff.py` reports **zero diffs** for Mario's Tennis
  bootstrap up to the first frame's XPEND interrupt.
- The diff tool, when armed earlier, shows exactly where the
  bootstrap touches each register and matches Beetle's state at
  every step. You see register-level visibility that *proves* the
  recompiled code is doing the same thing as the oracle, even
  before any pixel renders.

---

## Phase 4 — VIP renderer + SDL window (≈ 2-3 sessions) — **THIRD VISUAL MILESTONE: PIXELS**

**Goal:** `vb-runtime` opens its own SDL window and composites the
left/right framebuffers from the cart's BG segments + CHAR RAM +
WORLDS + OBJ groups, exactly as the real VIP does. The first frame
rendered must match Beetle's `framebuf_diff` byte-for-byte.

**Steps:**
- Implement the columnar renderer in `vip.c`:
  - For each frame: walk the WORLDS table top-down.
  - For each world: render its BG segment using CHAR RAM, applying
    parallax / line-table offsets.
  - Composite OBJ groups (sprites) on top per their priority.
  - Output to the L/R framebuffer at the documented VRAM addresses.
- Add SDL window + framebuffer pump in `main.cpp` — convert the VB
  4-level brightness to RGB and present at 50.27 Hz.
- Wire `tools/_framebuf_diff.py` against Beetle's framebuffer.

**Exit criteria:**
- `vb-runtime` SDL window shows Mario's Tennis title screen.
- `framebuf_diff` between `vb-runtime` (4390) and `vb-beetle`
  (4391) reports zero bytes different for the first 100 frames.

---

## Phase 5 — Mario's Tennis title screen end-to-end (≈ 1-2 sessions)

**Goal:** All of P3's bootstrap-only scope expands to the whole cart.
The recompiler emits clean C for every function in the ROM. The
runtime boots, the title screen renders, oracle matches for 5
seconds (250 frames).

**Exit criteria:**
- `vbrecomp --config games/mario_tennis.toml` emits clean C with
  no `vb_stub_abort` reachable from the recompiled code.
- `first_divergence` reports `frame >= 250`.

---

## Phase 6 — Mario's Tennis gameplay (≈ 2 sessions)

Inputs reach the game; ball physics matches oracle; sound is
produced (VSU Phase 2 implementation).

## Phase 7 — V-Tetris / Galactic Pinball (≈ 1 session each)

Second commercial title as cross-validation; finds any
Mario's-Tennis-specific assumptions baked into the recompiler.

## Phase 8 — Wario Land (≈ 3-4 sessions)

Hardest target. Includes save SRAM, more VIP feature usage, the
larger code base (2 MB cart). The shape of the project is proven
by getting all the way through Wario Land.

---

## Why this order, in one paragraph

The previous plan made P5 (Mario's Tennis title) the first time you'd
see a pixel — which is dishonest, because you don't actually have
visibility into what's working until the title renders. The revised
plan front-loads ground truth: Beetle VB is a real emulator that
already renders, so P2.5 gives you the visual reference. P3 produces
real executed C with no graphics yet, but P3.5 makes the *register
state* visible and diffable between our runtime and the oracle —
register-level visibility is just as valuable as pixel-level if you're
trying to find divergences. P4 then lights up our own framebuffer.
At every step the work is *real*: no stubbed renderers, no fake test
patterns, no "this would be where the title would go." Either we
render exactly what the cart's data programs us to render, or we
diverge from the oracle and the diff tells us where.

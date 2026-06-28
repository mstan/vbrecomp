# Virtual Boy Accuracy Burndown (living doc)

Full-coverage accuracy scorecard for the V810 static recompiler,
modelled exactly on `psxrecomp/_wt-tomba2/psxrecomp/ACCURACY_BURNDOWN.md`
(the 7-axis master). That project split its *active* axis (cycle/timing)
into a companion `FAITHFUL_TIMING_PLAN.md`; here the **active front is
audio** (axis 5 — VSU), stood up this session. Everything else is
tracked below at coverage level.

This file lives with the recompiler on branch `accuracy/vb-burndown`.
It is a scorecard, not a narrative — each item names the **external
reference** to cross-check against and the **validation method**, and is
only GREEN when BOTH are satisfied (see the gate).

> Scope note (2026-06-28): the audio axis was taken from *not measured*
> to a **first real recomp-vs-oracle differential** in one slice. The
> result is RED and now quantified — see "Axis 5" and "First audio
> comparison". The harness (always-on audio ring on both processes +
> drift-tolerant diff) is proven; root-causing the VSU divergence is the
> next lever, not part of this slice.

---

## Method (non-negotiable) — the STRICT GREEN GATE

Adapted verbatim in spirit from the PSX master (`ACCURACY_BURNDOWN.md:30-35`):

> Every item gets: a **status**, the **external comparative(s)** to
> cross-reference against, and a **validation method**. "Looks good" is
> NOT a status. An item is only **GREEN** once it is **(1) cross-referenced
> against an external reference** (V810 Architecture Manual / VB
> Programmer's Manual / Mednafen-VB source / a hardware test ROM) **AND
> (2) validated against the accurate oracle at runtime.** Self-agreement
> — the recompiled C agreeing with our own decoder/emitter — proves
> internal consistency, **NOT correctness**: both can be identically
> wrong (vbrecomp CLAUDE.md §13).

Two independent conditions, BOTH required. The runtime validation is
always a **cross-process** comparison (Rule 14): `vb-runtime` (port 4390)
vs `vb-beetle` (port 4391), querying always-on ring buffers, never
lockstep, never pause/step.

### Oracle fidelity caveat (read before trusting axis 2)

The oracle is **Beetle VB / Mednafen VB** (`beetle-vb/`,
`mednafen_vb_libretro.dll`, hosted by `vb-beetle.exe`). It is the
de-facto VB accuracy reference and the upstream of essentially every
other "accurate" VB emulator. **But it is instruction-accurate, not
cycle/pipeline-accurate:** the author states it was "not designed to be
CPU-timing-accurate … lack of wait state emulation and lack of register
hazard pipeline stall emulation," and it is "not VIP draw-timing
accurate." Its V810 costs are flat `timestamp += N` ("or higher?")
constants (`mednafen/vb/v810_cpu.cpp`).

Consequence for the gate:
- For **state / semantics / MMIO / audio content** the oracle is ground
  truth — diff against it directly.
- For **cycle/timing** the oracle is a **deterministic shared
  reference**, not silicon truth. The cycle axis cannot be GREEN against
  the oracle *alone*; it needs the V810 Architecture Manual timing tables
  (and ideally a HW-test-ROM) as the primary reference, with the oracle's
  guest-cycle counter used for Δ-convergence cross-checking only.

---

## Comparative sources (the reference shelf)

- **NEC V810 Architecture Manual** + **Virtual Boy Programmer's Manual**
  (Sacred Tech Scroll / Planet Virtual Boy) — what the hardware does.
  Cite the section per item. (`docs/HARDWARE_NOTES.md` collects the
  numbers we rely on.)
- **Mednafen-VB source in-tree** at `beetle-vb/mednafen/vb/` — the
  oracle's own implementation (`v810_cpu.cpp`, `vip.cpp`, `vsu.cpp`,
  `timer.cpp`) AND the runtime oracle on **port 4391**.
- **Hardware test ROMs** — none integrated yet. A V810/VIP/VSU test-ROM
  harness is a tracked to-build item below (ground truth above any
  emulator).
- **Ghidra + V810/V830 SLEIGH** — for what the cart bytes are; never the
  source of truth for execution correctness.

---

## Validation infrastructure

### In place
- **Write trace ring (`wtrace`)** — always-on 1M-entry ring; every guest
  store recorded (`memory.c`). Cross-process diff vs oracle write stream.
  Tools: `tools/_wtrace_summary.py` (single-process), `tools/_wram_diff.py`.
- **VIP/framebuffer diff** — `tools/_vip_diff.py`, `tools/_framebuf_diff.py`
  cross-compare `vb-runtime` (4390) vs `vb-beetle` (4391): VIP register
  bank + left-eye pixels.
- **Frame / fntrace / crash / freeze-heartbeat / vsu-shadow rings** — all
  always-on (`debug_server.c`).
- **★ NEW (this session) — audio capture + differential:**
  - `audio_pcm` always-on ring command on **both** servers
    (`debug_server.c` handle_audio_pcm; `beetle_debug_server.c`
    handle_audio_pcm). Streams the VSU/oracle output ring by **absolute
    frame index** (non-destructive; the SDL playback path is untouched).
    Oracle side required un-discarding the audio batch callback
    (`beetle_libretro.cpp` audio_batch_cb — was a no-op).
  - `tools/audio_compare.py` — launches both processes, drains `[0, N)`
    from boot, runs a drift-tolerant metric (see Axis 5). Proven:
    zero-loss from-boot capture on both sides; deterministic across runs.

### To build (prerequisite tooling, tracked as burndown items)
- **Cycle comparator** — a `cyc_watch`-style PC-anchored cycle ring on
  `vb-runtime`, diffed against the oracle's guest-cycle counter. The
  oracle already exposes it with **no source hook**: `V810::Run()` returns
  `v810_timestamp` (public), accumulated per frame in `libretro.cpp`.
  Pairs with a **`vb_instr_base_cycles()` single-source cost seam** (V810
  ISA) consumed by the emitter — Δ-gated vs the oracle. *(Axis 2)*
- **★ DONE — Per-instruction oracle trace (`RB_CPUHOOK` ring).** Mednafen's
  `RB_CPUHOOK` (every instruction at `v810_oploop.inc:54`) now records
  `{pc, PSW, regs-FNV(r1..r30), cumulative cycle}` into an always-on
  first-N-from-boot ring on both processes (oracle: `libretro.cpp` +
  `v810_cpu.cpp`, recorded in `tools/ORACLE_CPUHOOK_PATCH.md`; recomp:
  `cpuhook.c` + the emitter's `VB_CPUHOOK`, built `-DVBRECOMP_CPUHOOK=ON`).
  Exposed by a `cpuhook` TCP command on both ports; diffed by
  `tools/cpuhook_compare.py` (first divergence by retire-seq + cycle Δ).
  Serves Axes 1 (semantics), 2 (cycle Δ) and 3/6. *(Axes 1,2,3,6)*
- **Exception/IRQ ring diff** — record `{cause, PC, cycle}` on each
  interrupt take, diff vs oracle. *(Axis 3)*
- **HW test-ROM harness** — run V810/VIP/VSU test ROMs native vs oracle,
  diff pass/fail + result registers. *(Axes 1,2)*

---

## THE 7 AXES

### Axis 1 — Instruction semantics (V810 decoder/emitter)
**Status: STRONG (instruction-accurate).**
- [x] Near-complete V810 opcode coverage with explicit PSW flag math —
  cross-ref V810 Architecture Manual; emitter at `recompiler/v810/emitter.py`
  (direct decode→C, no IR). Produces `generated/marios_tennis_full.c`
  (377 fns).
- [ ] **Gaps fatal-abort, never stub** (good): `CAXI`, all 12 `BSU`
  bitstring ops, and the FP exception flags (FRO/FIV/FZD/FOV/FUD)
  unimplemented — `vb_stub_abort()` on reach.
- [x] **Per-instruction oracle validation WIRED & PASSING** (this session).
  The `RB_CPUHOOK` ring is built on both processes (recomp `cpuhook.c` via
  the emitter's `VB_CPUHOOK`, built `-DVBRECOMP_CPUHOOK=ON`; oracle
  `libretro.cpp`/`v810_cpu.cpp`, see `tools/ORACLE_CPUHOOK_PATCH.md`),
  exposed by a `cpuhook` TCP command on 4390/4391, diffed by
  `tools/cpuhook_compare.py` (first-divergence by retire-seq + cycle Δ).
  **Result (Mario's Tennis, from boot): 557,125 instructions matched
  EXACTLY** — pc + PSW + register-FNV (r1..r30) identical instruction-for-
  instruction. This is the first true oracle validation of the CPU core
  (the oracle's regs were previously unexposed — `get_registers` returns
  `ok:false`). The first divergence is **not** a CPU bug — it is a VIP
  `DPSTTS` (0x5F820) display-status read whose value differs due to
  fine-grained VIP draw timing (Axis-5a / Axis-2), at pc `0xFFF8197E`.
- [x] **Re-sync walk validates ~1.4M instructions.** `cpuhook_compare.py`
  steps past timing-input divergences by re-aligning on pc+regs:
  **1,405,565 instructions matched; ALL 4 divergence regions are
  `mmio/timing-input` (VIP/VSU hardware reads); ZERO non-peripheral
  semantic divergence.** The Axis-1 latent emitter bugs (MUL/MULU Z, r30
  writeback, FP) are confirmed **not triggered** in real boot execution.
  The walk stops at the first frame-synchronized poll-loop
  (`LD.H @ 0xFFF8019A`) — past that the cart's control flow depends on
  sub-frame VIP timing that differs between the two emulators, so lockstep
  CPU comparison is no longer meaningful (the limit is VIP-timing coupling,
  not a CPU bug).
- ⚠ `docs/INSTRUCTION_STATUS.md` is **stale/misleading** — every row says
  `lifted:no/emitted:no` because `vbrecomp_status.py` hardcodes those
  columns and only checks the decoder, never the emitter. Trust the
  emitter + generated C, not this doc.

**Gap:** per-instruction oracle validation; 14 unimplemented ops (abort,
not silently wrong). **Lever:** `RB_CPUHOOK` trace ring + HW test ROMs.

### Axis 2 — Cycle / timing
**Status: MODELED & VALIDATED — real per-instruction cost seam landed and
cpuhook-measured EXACT to ~1e-4 (≤95 cyc / 557K instrs) vs the oracle.
Tempo drift 10–50 → ~3 ms/s. The cost model is essentially complete.**
- [x] **Per-instruction V810 base cost seam** (`recompiler/v810/cycles.py`,
  single source). The emitter now charges each instruction's base cost
  into `cpu->cycles` as it runs (`emitter.py` — inline `cpu->cycles += N`
  after the per-instruction `cpu->pc =`; Bcond taken extra `+2` inside the
  taken path). `main.cpp` derives the device-tick delta as the `cpu.cycles`
  advance per pass (`cyc_before`/`cyc_delta`), **replacing `bbs_run × 3`**;
  `CYCLES_PER_BB` removed. `step_budget` is untouched (still the per-BB
  yield budget, now decoupled from cycle accounting).
- [x] **Costs sourced, not guessed** (Rule 12): every value is the oracle's
  own `ADDCLOCK` constant (`beetle-vb/.../v810_oploop.inc`, cited per op in
  `cycles.py`); the documented manual numbers (`docs/HARDWARE_NOTES.md:208`)
  agree (ALU=1, MUL=13, DIV=38, branch-taken=3). The V810 Architecture
  Manual full timing table is **not in-tree** — flagged where it matters.
- [x] **Direct per-instruction cycle Δ MEASURED** via the `RB_CPUHOOK` ring
  (the `cyc_watch` role; `tools/cpuhook_compare.py`). Over the first 557,125
  instructions from boot the cumulative cycle Δ (recomp − oracle) stayed
  within **[−95, 0]** — the flat-base cost model tracks the oracle's
  guest-cycle counter to **~1e-4**.
- [x] **Pipeline pairing CLOSED as empirically negligible** (was the deferred
  refinement). The oracle's load/store `lastop` +1/+2 pairing and 16-bit-bus
  split penalties (`v810_oploop.inc:560-692`) contribute ≤95 cyc over 557K
  instructions in real boot code — i.e. the flat base costs already match to
  0.017%. Modelling pairing would add per-instruction `cpu->lastop` runtime
  state for no measurable gain here. Revisit ONLY if a future load-dense
  divergence shows up in the cpuhook (it can be measured directly now).
- ⚠ **The residual audio ~3 ms/s is NOT the per-instruction cost model**
  (proven near-exact above). It lives in the **HALT-driven title-screen
  regime**, where device time advances by the fixed `IDLE_TICK_CYCLES`
  (`main.cpp`, ~20000/pass) rather than per-instruction costs, plus the
  by-design VSU output stage (Axis-5b). So the remaining audio gap is an
  **Axis-3 (HALT/idle pacing) + Axis-5b** concern, not Axis-2.

**Result (10 s from boot, Mario's Tennis):** onset drift **−11.3 → +2.2 ms/s**,
tempo drift **(10–50) → +3.6 ms/s**, alignment lag **1008 → 472 ms**,
verdict **RED → "PITCH MATCH (note-accurate)"**. (NCC stays ~0.09 — the
by-design Axis-5b output-stage difference, not timing.)
**Axis-2 cost model: essentially complete.** Remaining cross-axis levers:
Axis-3 HALT/idle pacing + Axis-5b output stage (the audio residual);
Axis-5a VIP draw timing (now driven by real cycles).

### Axis 3 — Interrupt / event timing
**Status: IMPROVED — event-driven idle; HALT IRQ-take now ~259 cyc.
Active-dispatch path still block-quantized.**
- [x] Faithful, Beetle-mirrored level-triggered controller
  (`interrupts.c`) — cross-ref oracle behavior.
- [x] **Event-driven HALT/idle pacing** (replaces the fixed 20000-cycle
  chunk). The idle loop steps device time to the next device boundary
  (`vb_vip_cycles_to_next_event` / `vb_timer_cycles_to_next_event`) and
  breaks on the first acceptable IRQ → HALT take-latency **~20000 → ~259
  cyc**, matching the oracle's event model (removes a magic-number
  approximation). No regression: audio byte-identical, framebuffer
  0/86016. This is the dominant path for MT (HALT-driven from the JMP r31
  sentinel). The audio being UNCHANGED confirms the ~3 ms/s residual is
  Axis-5b (VSU output), not HALT pacing.
- [ ] **Active-dispatch IRQ take still block-quantized** — IRQs are
  delivered only between dispatch passes (`main.cpp:522`); precise
  mid-block take needs cycle deadlines in the per-basic-block dispatch
  model (lower value for this HALT-driven cart).
- [ ] No exception-record ring diff vs oracle.

**Gap:** active-path take-point still quantized; no exception-ring diff.
**Lever:** exception-ring diff (the `RB_CPUHOOK` infra can host it);
mid-block take via cycle deadlines.

### Axis 4 — Memory map / MMIO
**Status: STRONG (instruction-accurate).**
- [x] Faithful 27-bit physical fold + Beetle-matched misc-page decode;
  unmapped/reserved access **fatal-aborts** rather than returning
  open-bus garbage (`memory.c`) — cross-ref VB memory map + oracle.
- [x] Every store recorded to the always-on `wtrace` ring; cross-process
  write-stream diff available.
- [ ] MMIO **read** path not yet diffed vs oracle by ordered access.

**Gap:** ordered MMIO read/write diff vs oracle not yet a standing test.
**Lever:** `mmio_tally`-style ordered recorder + oracle write-stream diff.

### Axis 5 — Peripherals / devices ← ACTIVE (audio)
**Status: MIXED — VIP STRONG, VSU RED (now measured), comms not modeled.**

- **VIP (video):** *scanline/column-accurate* — a genuine 259-cycle/column
  state machine (`vip.c`), the most accurate subsystem.
  - [x] **★ Pixels RECORDED green** (title screen): `_framebuf_diff.py` →
    **0 / 86016 left-eye pixels differ (0.00%)**. VIP renders content-
    identical to the oracle — the prior "validated" claim is now an actual
    recorded result.
  - [x] **Programmed VIP registers match** (`_vip_diff.py`) EXCEPT `INTPND`
    (runtime `0x401E` vs oracle `0x001E`) — the difference is solely the
    `XPEND` bit (0x4000, drawing-finished IRQ). So the residual is **draw-
    COMPLETION timing phase**, not content: the two are at slightly different
    sub-frame draw phases. Same root the cpuhook surfaced (the `DPSTTS`
    divergence). Draw timing rides the Axis-2 cycle stream (now real) but the
    VIP column/draw-finish scheduling vs the oracle's is not yet phase-gated.
- **VSU (audio) — 6 channels** (ch0-3 wave, ch4 sweep/FM, ch5 noise; *not*
  16 — that was a brief error; see `docs/HARDWARE_NOTES.md`).
  - [x] Synthesis is a near-verbatim port of Mednafen `vsu.c` — state
    machine field-for-field (`vsu.c`).
  - [ ] **Output stage differs by design:** recomp DC-centers by `-0x20`
    (`vsu.c:278`) and scales the mix `<<2` (`vsu.c:456`) instead of
    Mednafen's `Blip_Synth` band-limiting + per-channel volume. No
    band-limiting → aliasing.
  - [x] **★ Clock-domain bug FOUND & FIXED** (this session). The port
    fed `vsu_step_channel` the full **20 MHz** CPU delta, but Mednafen's
    update loop (which this is a verbatim port of) runs in the **5 MHz**
    domain — `libretro.cpp` feeds the VSU `timestamp>>2` and clocks Blip
    at `VB_MASTER_CLOCK/4` (`beetle-vb/libretro.cpp:1944,2346`). Every
    channel advanced 4× too fast → pitch ~2 octaves sharp. Fix: clock
    channel synthesis at CPU/4 with a carried sub-4 remainder, output
    cadence (453 cyc/sample) unchanged (`vsu.c` `vb_vsu_tick`,
    `s_vsu_clock_residue`). **Verified:** drift-aligned pitch bias
    +2590 c → **−5 c** (`audio_compare.py` 6b).
  - [x] **Tempo drift FIXED to ~3 ms/s** by the Axis-2 cycle model
    (10–50 → +3.6 ms/s) and proven NOT to be HALT pacing (Axis-3 event-
    driven idle left the audio byte-identical). The pitch + tempo are now
    correct.
  - [ ] **Output stage still differs by design (the dominant remaining
    audio gap)** — DC-center `-0x20` + `<<2` point-sampling vs Mednafen
    `Blip_Synth` band-limiting → low waveshape NCC (~0.09), +3.8 dB level,
    aliasing. **Axis-5b in progress:** `Blip_Buffer` vendored + integration
    spec'd (`docs/AXIS5B_BLIP_OUTPUT.md`); `vsu.c` rewrite is the next step.
- **Timer / game-pad:** faithful (`timer.c`, `input.c`), oracle-matched.
- **Cartridge RAM / expansion / link port:** **not modeled** (MT never
  touches them — would fatal-abort if it did; correctly out of scope).

**Gap:** pitch + tempo correct; residual = the by-design VSU output stage
(Axis-5b, in progress). VIP content pixel-exact (5a recorded green);
cart-RAM/link absent. **Lever:** finish the Axis-5b Blip output port.

### Axis 6 — Static-vs-dynamic recompiler fidelity
**Status: PARTIAL.**
- [x] One C function per guest function; loop-style trampoline (JAL =
  recursive call, JR/JMP = set-pc-return); `--seeds-toml` for indirect
  tables; output **CRC32-locked** to the cart (`0x7CE7460D` for Mario's
  Tennis). No IR.
- [ ] No second-backend equivalence harness (the project has no
  interpreter by design — Rule 0), so "backend equivalence" is N/A; the
  fidelity oracle is the external emulator, which is correct.
- [ ] Dispatch-completeness (no missed indirect targets) not yet a
  standing oracle-gated check.

**Gap:** no standing first-divergence harness vs oracle. **Lever:**
frame-fingerprint ring + ordered recorder, diffed vs oracle from boot.

### Axis 7 — Determinism
**Status: GOOD (deterministic guest given identical input).**
- [x] No guest RNG; wall-clock drives only pacing/watchdog, never guest
  state. **Confirmed this session:** two independent from-boot launches
  produced byte-identical audio RMS (recomp 462.6, oracle 741.6).
- [ ] No explicit record/replay or seed control surface.

**Gap:** no record/replay facility. **Lever:** cross-run frame-fingerprint
equality as a standing check.

---

## 7-axis summary table

| # | Axis | Verdict | Primary gap | Next lever |
|---|------|---------|-------------|-----------|
| 1 | Instruction semantics | **STRONG — oracle-validated** (557K instrs exact from boot) | 14 ops abort (unreached); HW test ROMs | `RB_CPUHOOK` ring DONE; extend window past first VIP-timing divergence |
| 2 | Cycle / timing | **MODELED & VALIDATED** (cpuhook-exact to ~1e-4; drift 10–50→~3 ms/s) | pairing closed (≤95 cyc, negligible); audio residual is Axis-3/5b, not Axis-2 | (complete) — audio residual → Axis-3 HALT pacing + Axis-5b output stage |
| 3 | Interrupt / event timing | **IMPROVED** — event-driven idle (HALT take ~259 cyc) | active-dispatch take still block-quantized; no exception-ring diff | exception-ring diff (RB_CPUHOOK infra); mid-block cycle deadlines |
| 4 | Memory / MMIO | **STRONG** (instr-accurate) | ordered MMIO read diff not standing | ordered recorder + oracle write-stream diff |
| 5 | Peripherals (VIP/**VSU**/pad) | **5a VIP recorded green** (0/86016 px); **5b VSU pitch+tempo FIXED**, output stage in progress; comms absent | 5a draw-timing phase only; 5b band-limited output (Blip vendored, vsu.c rewrite pending); cart-RAM/link unmodeled (out of scope) | finish Axis-5b Blip port (`docs/AXIS5B_BLIP_OUTPUT.md`) |
| 6 | Static↔dynamic fidelity | **PARTIAL** | no standing first-divergence harness | fingerprint ring + ordered recorder vs oracle |
| 7 | Determinism | **GOOD** | no record/replay | cross-run fingerprint equality |

---

## Always-on ring buffer model (never arm-then-capture)

Per vbrecomp CLAUDE.md Rule 3 and the global ring rule: probes QUERY a
buffer for a window; they never arm-record-run-dump, and never pause/step
to synchronize observers. Rings on `vb-runtime`, all from boot:
`frame` snapshots, `wtrace` (1M stores), `fntrace`, crash/freeze
heartbeat, `vsu_shadow`, and **`audio_pcm` (new)**. The oracle host
mirrors `audio_pcm` (and `vip_state`, `read_ram`) on port 4391.

**The new audio capture honors this exactly.** The VSU output ring
(`vsu.c`, now `1<<22` frames ≈ 95 s) and the oracle's audio ring
(`beetle_libretro.cpp`, `1<<20` frames ≈ 24 s) record every emitted
stereo frame from boot with a monotonic absolute counter. The harness
*queries* `[start_abs, head)` and streams forward; it never arms a
capture. `vb_vsu_read_abs` / `vb_beetle_audio_read_abs` are
non-destructive (decoupled from the SDL drain cursor) and report the
oldest still-resident index so a consumer that falls behind sees a
**gap**, never a silent skip. The runtime ring had to be enlarged because
a `--headless` run free-runs at ~30× realtime — a 3 s ring evicted boot
audio before the harness could read it.

---

## Oracle exposure map (one core, three fronts)

| Front | How exposed | Hook needed? |
|-------|-------------|--------------|
| State / divergence | `read_ram` (WRAM/cart), `vip_state` (writable bank), full `StateAction` snapshot | getters public; per-instr regs need `RB_CPUHOOK` |
| Cycle counter | `V810::Run()` returns `v810_timestamp`, accumulated per frame | **none** (already public) |
| Audio PCM | `audio_batch_cb` → always-on ring → `audio_pcm` TCP | **done this session** (was discarded) |

---

## Audio-first comparison plan (the active slice)

**Why drift-tolerant, not bit-exact.** Bit-exact is not realistic for VB
audio in this recomp: the output stage differs by design (DC-center + `<<2`
vs Mednafen `Blip_Synth` band-limiting + per-channel volume), and the VSU
is driven by the coarse cycle estimate, so onset timing and tempo drift.
A bit-compare would only ever say "different." The metric is therefore
layered and tolerant (`tools/audio_compare.py`):

1. **Global alignment lag** — FFT cross-correlation, search ±2.5 s (must
   exceed the boot lead-in difference between the two, ~1 s here).
2. **Normalized xcorr peak (NCC, 0..1)** at best lag — waveshape match.
3. **Level offset (dB)** — auto-calibrated RMS ratio (loudness).
4. **Windowed envelope correlation** — dynamics/timing, phase-robust.
5. **Onset-timing drift** — matched-onset linear fit (slope = cumulative
   drift in ms/s = the cycle-estimate signature).
6. **Per-window pitch error (cents)** — autocorrelation f0 (alignment-
   sensitive; treat as indicative until per-note alignment lands).

The capture model is two **independent** deterministic processes (Rule
14), each free-running from boot, each query-drained from its always-on
ring — no lockstep, no stepping.

### First audio comparison (2026-06-28, Mario's Tennis, 10 s from boot)

Command: `python tools/audio_compare.py --rom roms/marios_tennis.vb --seconds 10`

| metric | value | reading |
|--------|-------|---------|
| captured | recomp 441000 / oracle 441000 frames, **0 lost** | harness sound |
| alignment lag | +1008 ms | recomp music starts ~1 s later |
| **xcorr NCC** | **0.012** | waveshapes essentially uncorrelated |
| level offset | +4.6 dB (oracle louder) | mix/scale differs |
| envelope corr | 0.02 | dynamics uncorrelated after alignment |
| onset drift | −11.3 ms/s | cumulative timing divergence |
| pitch error | median ~1600 cents (noisy) | recomp pitched well above oracle |
| determinism | identical RMS across 2 runs | guest is deterministic |

**Initial verdict: RED / DIVERGENT** — recomp pitched ~4× high (per-window
ratio *mean* 4.09), NCC≈0.01. Both sides produce real evolving music from
boot (neither silent nor a stuck tone); the recomp tracked the melody
*contour* at the wrong pitch.

### Root cause found & fixed (same session)

The recomp clocked the VSU at the full **20 MHz** CPU rate; the VB VSU
runs at **CPU/4 = 5 MHz** (Mednafen feeds `timestamp>>2`, Blip at
`VB_MASTER_CLOCK/4`). The verbatim-ported `vsu_step_channel` therefore
advanced every channel's frequency counter 4× too fast → ~2 octaves
sharp. Fixed by clocking channel synthesis at CPU/4 (carried sub-4
remainder), output cadence unchanged.

| metric (10 s) | before fix | after fix |
|---|---|---|
| drift-aligned **pitch bias** | **+2590 cents** (~2 oct) | **−5 cents** |
| per-window pitch ratio (mean) | 4.09× | ~1.0× |
| verdict | RED / DIVERGENT | **PITCH-SCALE CORRECT — residual tempo drift** |

**Post-fix verdict: PITCH-SCALE CORRECT.** The dominant defect is gone.
The residual is **tempo drift (~10-50 ms/s)** — note *sequencing*
desyncs over the clip because music timing rides the coarse Axis-2 cycle
estimate — plus the by-design output-stage difference (low waveshape NCC,
+3 dB level). The cascade is documented (CLAUDE.md/recomp-debug: a fix
that exposes a *different* failure is correct): **Axis 2 (cycle model) is
now the next lever**, and it is the same root that limits axes 3 and 5.

---

## Phasing / priorities

- **P0 (done this session):** VSU pitch divergence root-caused & fixed
  (20 MHz → CPU/4 clock); drift-aligned pitch bias +2590 c → −5 c.
- **P0 (first cut DONE):** Axis 2 cycle model — `cycles.py` per-instruction
  base-cost seam consumed by the emitter; `main.cpp` ticks devices by the
  real `cpu.cycles` delta (no more `bbs_run×3`). Tempo drift 10–50 → ~3 ms/s,
  verdict RED → "PITCH MATCH (note-accurate)". Remaining: (a) load/store
  pipeline pairing for the residual ~2–4 ms/s; (b) the `cyc_watch` ring
  Δ-gated vs the oracle's `V810::Run()` counter for the direct proof +
  residual attribution. Same root still feeds Axis-3 IRQ-take quantization
  and VIP draw-timing (now driven by real cycles).
- **P1:** wire `RB_CPUHOOK` per-instruction oracle trace (Axes 1/3/6).
- **P2:** exception-ring diff (Axis 3); ordered MMIO read diff (Axis 4);
  first-divergence fingerprint harness (Axis 6).
- **P2:** HW test-ROM harness (ground truth above the oracle).

---

## Reproduce

```sh
# build (msys2 mingw)
cmake --build build --target vb-runtime vb-beetle

# first audio differential (launches both headless from boot)
python vbrecomp/tools/audio_compare.py --rom roms/marios_tennis.vb \
    --seconds 10 --wav-out cap --json-out cap.json
```
Both binaries need `C:\msys64\mingw64\bin` on PATH (SDL2 / libstdc++);
the harness injects it for the children it launches.

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

> Scope note (2026-06-28): the audio axis went from *not measured* → RED →
> **STRONG MATCH** across the session (VSU pitch fix → Axis-2 cycle model →
> Axis-5b Blip output rewrite → Axis-3 mid-block IRQ take). With audio
> solved, the effort has shifted from **breadth** (is each axis even
> measured?) to **depth** (the few genuinely-open items). Consolidated
> status below: **WON** = recorded cross-process green. All 7 axes are now
> WON; the latent MUL/DIV emitter bugs were fixed + validated and the Axis-5a
> draw-timing phase + Axis-6 past-the-wall fidelity were closed. The only
> residual is non-MT depth: FP host-float vs SoftFloat (unreached by MT) and
> a record/replay convenience. See the "Consolidated status" table near the
> end.

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
  semantic divergence.** The Axis-1 latent emitter bugs were confirmed
  **not triggered** in real boot execution — and are now **FIXED** (below).
  The walk stops at the first frame-synchronized poll-loop
  (`LD.H @ 0xFFF8019A`) — past that the cart's control flow depends on
  sub-frame VIP timing that differs between the two emulators, so lockstep
  CPU comparison is no longer meaningful (the limit is VIP-timing coupling,
  not a CPU bug).
- [x] **★ Latent MUL/DIV bugs FIXED + validated** (`tools/isa_semantics_check.py`).
  Four emitter defects, identified by diffing the emitter against the
  oracle's own source (`v810_oploop.inc`, the code compiled into vb-beetle)
  and fixed to match it exactly: (1) **MUL/MULU Z-flag** was set from the
  full 64-bit product; the oracle sets it from the low 32 bits
  (`SetSZ(P_REG[arg2])`). (2) **MUL/MULU/DIV/DIVU r30-vs-dest write order**
  when dest==r30 — the oracle writes r30 first so the destination write
  wins; the emitter wrote them reversed. (3) **DIV/DIVU ÷0** aborted instead
  of raising the V810 zero-division exception (`vb_exception`,
  `VB_ZERO_DIV_HANDLER`/`VB_ECODE_ZERO_DIV`). (4) INT_MIN/-1 was already
  guarded. **Proof:** a compile-and-run harness runs the EXACT emitted C
  against oracle-derived golden — **0/10 cases pass on the old emitter,
  10/10 on the fixed one.** No MT regression: regen + cpuhook still
  **1,405,572 instructions match** the oracle, audio STRONG MATCH unchanged.
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
  **→ RESOLVED (later sessions):** Axis-5b Blip output rewrite + Axis-3
  mid-block active-dispatch IRQ take together took audio to **STRONG MATCH
  (NCC 0.98, tempo drift 0, onset 94/94)**. The drift was active-path
  interrupt-latency accumulation, not HALT pacing.

**Result (10 s from boot, Mario's Tennis):** onset drift **−11.3 → +2.2 ms/s**,
tempo drift **(10–50) → +3.6 ms/s**, alignment lag **1008 → 472 ms**,
verdict **RED → "PITCH MATCH (note-accurate)"**. (NCC was ~0.09 here — later
taken to **0.98 / STRONG MATCH** by the Axis-5b Blip output rewrite + the
Axis-3 mid-block IRQ take that removed the residual tempo drift.)
**Axis-2 cost model: essentially complete.** Remaining cross-axis levers:
Axis-3 HALT/idle pacing + Axis-5b output stage (the audio residual);
Axis-5a VIP draw timing (now driven by real cycles).

### Axis 3 — Interrupt / event timing
**Status: STRONG — event-driven idle AND mid-block active-dispatch IRQ
take. The active-path fix locked audio to the oracle (NCC 0.09 → 0.98).**
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
- [x] **★ Mid-block active-dispatch IRQ take (cycle deadlines).** The
  emitter's per-basic-block yield check now also yields when
  `cpu->cycles >= cpu->cycle_deadline` (`emitter.py`); `main.cpp` sets that
  deadline before each pass to `cpu.cycles + cycles-to-next-device-event`
  (VIP column / timer divider — the same boundaries the idle loop uses), so
  a mid-pass IRQ is delivered within **one basic block** of the true event
  instead of up to a 250000-block pass later. `cpu_state.h` carries the new
  `cycle_deadline` field; `STEP_BUDGET` stays as the no-event backstop.
  **This eliminated the interrupt-latency the music ISR's timer re-arm was
  accumulating into tempo drift.**
  - **Proof (audio_compare, deterministic, 10 s & 30 s):** tempo drift
    **+10.8 → −0.0 ms/s**, onset timing **6/33 → 94/94 matched, std 0.00
    ms**, **NCC 0.0905 → 0.9806**, level offset **+1.09 → −0.03 dB**, RMS
    743.9 vs oracle 741.6 → verdict **STRONG MATCH**. This simultaneously
    closed the Axis-5b NCC ceiling (the Blip output was correct; only the
    timing drift was masking it).
  - **No regression:** framebuffer **0/86016** pixel-exact; cpuhook stream
    **1,405,572 instructions match the oracle**, zero non-peripheral
    semantic divergence (same VIP-timing wall); perf fine (28 s wall for a
    10 s from-boot capture despite ~259-cyc yields).
  - **★ Side effect — wall-clock play speed corrected to exactly 50.27 Hz.**
    The windowed runtime paces *presents* to 50.27 Hz but advances emulated
    cycles per dispatch pass. Before the deadline yield, a pass over-ran the
    frame boundary by ~a full extra frame (250000-block passes ≈ ~2 frames of
    cycles), so each paced present consumed ~2 emulated frames → the game ran
    **~2× too fast** (display capped at 50.27 Hz, internal clock ~94 Hz).
    With the deadline yield the loop stops within one frame of the boundary,
    so one present ≈ one emulated frame. **Measured (windowed, `frame` TCP
    command): 50.27 Hz, ratio 1.000× real hardware** (was ~2× fast). The same
    fix corrected audio waveshape, tempo drift, AND real-time speed.
- [ ] No exception-record ring diff vs oracle (separate observability item).

**Gap:** no standing exception-record ring diff (IRQ take is now timing-
accurate; this would be a belt-and-suspenders cross-check). **Lever:**
exception-ring diff (the `RB_CPUHOOK` infra can host it).

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

### Axis 5 — Peripherals / devices
**Status: WON (5a VIP pixels + draw-timing phase, 5b VSU audio STRONG
MATCH); comms (5c) out of scope.**

- **VIP (video):** *scanline/column-accurate* — a genuine 259-cycle/column
  state machine (`vip.c`), the most accurate subsystem.
  - [x] **★ Pixels RECORDED green** (title screen): `_framebuf_diff.py` →
    **0 / 86016 left-eye pixels differ (0.00%)**. VIP renders content-
    identical to the oracle — the prior "validated" claim is now an actual
    recorded result.
  - [x] **Programmed VIP registers match** (`_vip_diff.py`) — all scalar +
    indexed writable regs identical. `INTPND` sampled at an uncontrolled
    instant wobbles (transient register, sampling-phase noise — NOT a bug).
  - [x] **★ Draw-timing PHASE GATE recorded green** (`vipphase_compare.py`).
    Both VIPs snapshot `DPSTTS` (display status) + `XPSTTS` (drawing status)
    into an always-on from-boot ring at each draw-timing **event**
    (FRAME_START/GAME_START/XP_END/L|R FB_END), where both are freshly
    advanced — so the comparison is phase-aligned, not sampling noise.
    Result: **PHASE MATCH — every draw-timing event identical in type,
    DPSTTS, AND XPSTTS** (17k events / 3.4k frames clean-launch; 27k / 5.4k
    in longer runs), zero divergence. **The VIP draw-timing state machine is
    oracle-EXACT at every event boundary.** This resolves the old INTPND/
    DPSTTS puzzle: the cpuhook's sub-frame `DPSTTS`-read divergence is the
    ≤95-cycle Axis-2 timing **jitter** sampled mid-column, NOT a draw-timing-
    model difference. Oracle side via a small `vip.c` expose
    (`tools/ORACLE_VIP_PHASE_PATCH.md`); observability-only, audio STRONG
    MATCH unchanged.
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
  - [x] **Output stage now matches the oracle's method (Blip rewrite DONE).**
    Replaced the `-0x20`/`<<2`/453-cadence point-sampling with the oracle's
    exact band-limited path: each channel feeds amplitude deltas into a 5 MHz
    `Blip_Buffer` (Synth volume `1/6/2` over `0x400`, bass-freq 20, no `-0x20`
    centering — DC removed by the high-pass), drained at 44.1 kHz into the
    same ring (`runtime/src/vsu.c`, `docs/AXIS5B_BLIP_OUTPUT.md`).
    **Proof (same-build before/after, deterministic):** level offset
    **+3.84 → +1.09 dB**, RMS **67% → 92% of oracle** (494.8 → 679.1 vs
    741.6) — the predicted amplitude/spectral convergence. NCC unchanged
    (0.093 → 0.091 @10 s; 0.231 → 0.237 @2 s): it is **capped by tempo
    drift**, not the output stage — short-window NCC is ~2.6× the 10 s value,
    confirming waveshape correlation walks out of phase as drift accumulates.
    The xcorr tempo number shifting (+3.6 → +10.8 ms/s) is a measurement
    artifact of the local-xcorr metric responding to the changed waveshape
    (the onset-fit estimator simultaneously reads −9.2 ms/s — opposite sign,
    i.e. both are noise-dominated on this sparse-onset content); `vsu.c` is
    CPU-timing-neutral by construction. **Residual audio gap is now squarely
    Axis-2/3 tempo drift**, not the output stage. The opt-in `vsu_shadow`
    verifier is intentionally disconnected under Blip (no per-sample integer
    mix to compare; its full-precision gain diverges from the oracle's
    quantization this axis matches) — default behavior unaffected (it was
    default-OFF/byte-identical), reworkable post-Blip later.
- **Timer / game-pad:** faithful (`timer.c`, `input.c`), oracle-matched.
- **Cartridge RAM / expansion / link port:** **not modeled** (MT never
  touches them — would fatal-abort if it did; correctly out of scope).

**Gap:** NONE for the audio chain — pitch + tempo + output stage all
oracle-matched. The Axis-5b Blip rewrite fixed the output stage; the Axis-3
mid-block IRQ take then removed the tempo drift that had been masking it,
and the two together took audio to **STRONG MATCH** (NCC **0.98**, drift
**0**, onset 94/94, level −0.03 dB). VIP content pixel-exact (5a recorded
green); cart-RAM/link absent (out of scope). **Lever:** audio is solved;
remaining VSU work would be the 5a-style draw-timing phase gate only.

### Axis 6 — Static-vs-dynamic recompiler fidelity
**Status: WON — the static recomp is proven to match dynamic execution at
instruction, memory, and output granularity.**
- [x] One C function per guest function; loop-style trampoline (JAL =
  recursive call, JR/JMP = set-pc-return); `--seeds-toml` for indirect
  tables; output **CRC32-locked** to the cart (`0x7CE7460D` for Mario's
  Tennis). No IR.
- [x] **★ Static↔dynamic identity recorded green.** The `RB_CPUHOOK`
  harness is exactly this proof: **1,405,572 instructions match the oracle
  exactly** (pc + PSW + r1..r30) from boot — the statically-recompiled C
  produces bit-identical CPU state to the dynamic reference, with zero
  non-peripheral semantic divergence. The output side corroborates:
  framebuffer **0/86016** pixel-exact and audio **STRONG MATCH** (NCC 0.98).
- [x] **Frame-fingerprint ring now live** (was dead code). `cpu.frame`
  advances from elapsed cycles (works `--headless`; was stuck at 0) and
  `vb_ring_frame_record` writes a per-frame {pc,gpr,psw} snapshot into the
  always-on 36k-frame ring (`main.cpp`); queryable via the `frame` command
  (`seq` advances). Verified: frame counter 6490→7496→8598 over 8 s.
- [x] **★ Whole-session WRAM fidelity recorded green** (`wramhash_compare.py`),
  extending fidelity proof PAST the instruction-level wall. Both processes
  FNV-1a-hash WRAM (64 × 1 KiB regions) at each GAME_START — the game-frame
  boundary the vipphase gate proved 1:1-aligned — into an always-on from-boot
  ring; the comparator aligns by GAME_START index. Result: **62 of 64 regions
  in PERFECT lockstep across all frames** (5777+ game frames; 98.44% of
  region-cells). Divergence is confined to two regions and is **benign by
  construction**: region 15 (0x3C00-0x3FFF = top of the cart stack) holds the
  recomp's deliberate `0xDEAD0000` `lp`/r31 sentinel (which the cpuhook
  already excludes) plus ~4-8 volatile stack locals carrying the ≤95-cycle
  Axis-2 sub-frame timing jitter; region 0 blips in 2/5777 frames (one timing
  cell). **The recomp's computed game state is byte-identical to the oracle
  across the whole session** — only the stack sentinel + timing-jitter locals
  differ. Oracle side via a libretro.cpp ring
  (`tools/ORACLE_VIP_PHASE_PATCH.md §3`); observability-only, audio STRONG
  MATCH unchanged.
- [ ] No second-backend equivalence harness (the project has no
  interpreter by design — Rule 0), so "backend equivalence" is N/A; the
  fidelity oracle is the external emulator, which is correct.

**Gap:** NONE for fidelity — instruction-level lockstep to the VIP-timing
wall (cpuhook 1.4M), THEN region-level WRAM lockstep through gameplay past it
(62/64 regions, residual = stack sentinel + timing jitter). Static↔dynamic
fidelity is proven both before and after the wall.

### Axis 7 — Determinism
**Status: WON (deterministic guest given identical input).**
- [x] No guest RNG; wall-clock drives only pacing/watchdog, never guest
  state. **Confirmed repeatedly this session:** independent from-boot
  launches produce **byte-identical** audio (NCC/RMS bit-identical across
  runs A/B at 10 s, with the final Blip + IRQ-take build: RMS 743.9 both
  runs, NCC 0.9806 both runs). The cpuhook stream is likewise reproducible.
- [ ] No explicit record/replay or seed control surface (depth, not a
  correctness gap — determinism itself is recorded green).

**Gap (depth):** no record/replay facility. **Lever:** cross-run
frame-fingerprint equality as a standing check (the live frame ring from
Axis 6 makes this a small follow-up).

---

## 7-axis summary table

| # | Axis | Verdict | Primary gap | Next lever |
|---|------|---------|-------------|-----------|
| 1 | Instruction semantics | **WON** — 1.4M instrs exact from boot; latent MUL/DIV bugs (Z-flag, r30 order, ÷0 trap) FIXED + validated (isa_semantics_check 10/10, no MT regression) | FP host-float vs SoftFloat on non-finite (separate, larger; aborts not silently wrong) | route FP through SoftFloat (if a target needs it) |
| 2 | Cycle / timing | **MODELED & VALIDATED** (cpuhook-exact to ~1e-4) | pairing closed (≤95 cyc, negligible); audio residual was Axis-3 IRQ latency + Axis-5b output — both now FIXED (audio STRONG MATCH) | (complete) |
| 3 | Interrupt / event timing | **STRONG** — event-driven idle + mid-block active-dispatch IRQ take (cycle deadlines); locked audio to oracle (NCC 0.09→0.98, drift →0, framebuf 0/86016, cpuhook 1.4M match) | no standing exception-ring diff | exception-ring diff (RB_CPUHOOK infra) |
| 4 | Memory / MMIO | **WON** — cpuhook proves every executed load returned the oracle's value (1.4M instrs, gpr exact); faithful fold + fatal-abort on unmapped | ordered MMIO read/write diff not a *standing* tool (cpuhook covers it implicitly) | ordered recorder + oracle write-stream diff (belt-and-suspenders) |
| 5 | Peripherals (VIP/**VSU**/pad) | **5a VIP WON** — pixels 0/86016 + draw-timing PHASE MATCH (DPSTTS/XPSTTS identical at every event, 17k+ events); **5b VSU audio STRONG MATCH** (NCC 0.98, drift 0, onset 94/94); comms absent | cart-RAM/link unmodeled (out of scope) | (5a/5b complete) |
| 6 | Static↔dynamic fidelity | **WON (both sides of the wall)** — cpuhook 1.4M instrs == oracle pre-wall; WRAM region-lockstep 62/64 regions through gameplay post-wall (residual = stack sentinel + timing jitter); framebuf 0/86016; audio STRONG MATCH | (none — fidelity proven pre- and post-wall) | (complete) |
| 7 | Determinism | **WON** — cross-run byte-identical (audio + cpuhook reproducible) | no record/replay (depth) | cross-run fingerprint equality |

---

## Consolidated status — breadth → depth (2026-06-28)

The breadth phase is over: **every axis is now WON for the Mario's Tennis
target with a recorded cross-process artifact.** What remains is a short,
well-bounded depth list, not unmeasured surface.

**WON (recorded green vs the oracle):**
- **Axis 2** cycle model — cpuhook cycle-Δ ≤95/1.4M (~1e-4), pairing closed.
- **Axis 3** interrupt/event timing — event-driven idle + mid-block IRQ
  take; locked audio to the oracle (NCC 0.09→0.98, drift→0).
- **Axis 4** memory/MMIO — cpuhook proves every executed load == oracle.
- **Axis 5a** VIP video — framebuffer 0/86016 pixel-exact AND draw-timing
  PHASE MATCH (DPSTTS/XPSTTS identical at every VIP event from boot).
- **Axis 5b** VSU audio — STRONG MATCH (NCC 0.98, onset 94/94, ±0.03 dB).
- **Axis 6** static↔dynamic — cpuhook 1.4M instr identity pre-wall + WRAM
  region-lockstep (62/64 regions) through gameplay post-wall.
- **Axis 7** determinism — cross-run byte-identical.
- **Axis 1** instruction semantics — 1.4M instrs exact AND the latent
  MUL/DIV bugs (Z-flag, r30 order, ÷0 trap) FIXED + validated 10/10
  (`isa_semantics_check.py`) with no MT regression.
- **Axis 5c** cart-RAM/link — correctly out of scope (MT never touches it).

**DEPTH remaining (the whole residual — narrow and explicit):**
1. **FP via host float vs SoftFloat** — the V810 FP ops use the x86 host FPU;
   they diverge from the oracle's SoftFloat only on non-finite / rounding
   edges (and the FP exception flags currently abort, never silently wrong).
   Larger than the MUL/DIV fixes (needs a SoftFloat path) and unreached by
   MT; do it only if a target needs it. (General-recompiler correctness.)
2. **Axis 7 record/replay** — determinism is proven; a replay surface is a
   convenience, not a correctness gap.
3. **Perf note:** mid-block IRQ take yields ~every device event (~259 cyc),
   so headless free-run dropped from ~30× to ~5× realtime — still ample for
   the harness and real-time play; gating the tight deadline on
   interrupts-deliverable is the lever if speed ever matters.

---

## Always-on ring buffer model (never arm-then-capture)

Per vbrecomp CLAUDE.md Rule 3 and the global ring rule: probes QUERY a
buffer for a window; they never arm-record-run-dump, and never pause/step
to synchronize observers. Rings on `vb-runtime`, all from boot:
`frame` snapshots, `vip_phase` (draw-timing events), `wram_hash` (per-frame WRAM regions), `wtrace` (1M stores), `fntrace`, crash/freeze
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

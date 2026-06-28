# Axis-5b — band-limited VSU output (Blip) integration plan

Goal: replace the recomp's point-sampled VSU output (DC-center `-0x20` +
`<<2` mix sampled every 453 cyc) with Mednafen's delta-fed `Blip_Synth`
band-limiting, so the audio waveshape matches the oracle (audio NCC ~0.09 →
toward the envelope ceiling). Faithful-DEFAULT fix (converges to the
reference + removes a recomp-introduced aliasing artifact), per the research
in this burndown.

## Status
- [x] **Blip_Buffer vendored** — `runtime/include/blip/Blip_Buffer.h` +
  `runtime/src/Blip_Buffer.c` (Mednafen 0.4.1, verbatim except an `INLINE`
  fallback `#define`), wired into `runtime.cmake`, compiles+links clean.
- [x] **`vsu.c` integration DONE** (this plan). `vsu_update_channel` feeds
  boundary + per-chunk deltas into `s_synth`/`s_bb_l`/`s_bb_r`; `vb_vsu_tick`
  accumulates a relative VSU-frame timestamp and flushes (`end_frame` +
  `read_samples`) every `VSU_FLUSH_CLOCKS` (16384) cycles into the existing
  ring. `-0x20`, `<<2`, and the 453-cadence are gone. `vsu_shadow` is
  disconnected under Blip (see decision below). **Result:** see Proof.

## Reference (oracle): `beetle-vb/mednafen/vb/vsu.c`
- `VSU_Init(bb_l, bb_r)` → `Blip_Synth_set_volume(&Synth, 1.0/6/2, 0x400)`
  (vsu.c:77-84). Buffers' clock rate = `VB_MASTER_CLOCK/4` (the 5 MHz VSU
  domain), sample rate 44100 — set by the host (`beetle-vb/libretro.cpp`
  sbuf setup; confirm the exact `VB_MASTER_CLOCK` constant there).
- `VSU_CalcCurrentOutput` (vsu.c:251-287): per channel, `WD * (Env*Level>>3
  +1)` with **NO `-0x20`** (the `- 0x20` is commented out) — DC is removed
  by delta-encoding, not a static center.
- `VSU_Update(timestamp)` (vsu.c:289-469): for each of 6 channels —
  1. feed the boundary delta at `last_ts`:
     `Blip_Synth_offset(&Synth, last_ts, left-last_output[ch][0], bb_l)` (+R)
     then update `last_output[ch]` (vsu.c:300-304);
  2. step the channel over `clocks = timestamp-last_ts` in chunks bounded by
     the clock dividers (the EXACT loop the recomp's `vsu_step_channel`
     already is), and **after each chunk** recompute the output and feed the
     delta at the advanced `running_timestamp` (vsu.c:456-464).
  3. `last_ts = timestamp`.
- `VSU_EndFrame(timestamp)` = `VSU_Update(timestamp); last_ts = 0;`. The host
  then `Blip_Buffer_end_frame(&sbuf, timestamp>>2)` + `Blip_Buffer_read_samples`.

## Recomp side today: `runtime/src/vsu.c`
- `vsu_step_channel(ch, n)` (~290-430) = Mednafen's per-channel inner loop,
  verbatim — but feeds NO Blip deltas.
- `vsu_emit_one_sample()` (461-515): sums all 6 channels' current output,
  `mix <<= 2`, clips S16, writes one stereo frame to the ring. Point-sampled.
- `vb_vsu_tick(cpu_cycles)` (517-544): clocks channels at CPU/4 via
  `s_vsu_clock_residue`, emits a sample every `VSU_CYCLES_PER_SAMPLE=453` CPU
  cyc.

## Integration design
1. **Statics** in vsu.c: `Blip_Buffer s_bb_l, s_bb_r; Blip_Synth s_synth;
   int s_last_output[6][2];` (+ include `blip/Blip_Buffer.h`).
2. **Init** (`vb_vsu_init`): `Blip_Buffer_init` both; `set_sample_rate(44100,
   msec)`; `Blip_Buffer_set_clock_rate(VB_MASTER_CLOCK/4)`;
   `Blip_Synth_set_volume(&s_synth, 1.0/6/2, 0x400)`; clear buffers; zero
   `s_last_output`. (Define the clock to match the recomp's existing 5 MHz
   VSU domain so pitch — already correct from the CPU/4 fix — is preserved.)
3. **Feed deltas**: give `vsu_step_channel` a running-timestamp param and add
   the boundary feed (before the loop) + the per-chunk feed (after each chunk
   advance), mirroring vsu.c:300-304 and 456-464. Output computed by a
   delta-form of `vsu_channel_output` WITHOUT the `-0x20` (drop the DC
   center) and WITHOUT `<<2` (the Synth volume replaces it).
4. **Rewrite `vb_vsu_tick`** to the `VSU_Update` shape: convert `cpu_cycles`
   to VSU (5 MHz) cycles via the existing `s_vsu_clock_residue`; treat each
   tick as a Blip frame at relative time `[0, vsu_cycles)`; for each channel
   feed+step over the interval; then `Blip_Buffer_end_frame(&s_bb_l/r,
   vsu_cycles)` and `Blip_Buffer_read_samples` L/R into a temp, writing
   stereo frames to the existing ring (`s_ring_l/r`, `s_ring_total`,
   eviction). Drop `vsu_emit_one_sample` + the 453-cadence + `s_sample_cycle_
   residue`.

## Risks / decisions
- **vsu_shadow coupling — DECIDED: disconnected under Blip.** The opt-in
  `vsu_shadow` verifier (default OFF) compared a per-sample full-precision
  float mix against the canon per-sample *integer* mix. Band-limited output
  has no per-sample integer mix, and the shadow's full-precision gain
  deliberately DIVERGES from the oracle's `(envelope*level>>3)+1`
  quantization — which is exactly what this output stage is matching. So the
  shadow is not wired into the Blip path; its source stays and
  `vb_vsu_init` still resets its status ring, so a future *post-Blip* rework
  can re-engage it as an overrides-style opt-in. Faithful default behavior
  is unaffected (the shadow was default-OFF and byte-identical when off).
- **Determinism**: `Blip_Synth_offset_resampled` is pure integer; volume is a
  one-time `double` at init. Must stay byte-identical across runs (verify).
- **Headless ring**: keep the same `s_ring` + `audio_pcm`/`vb_vsu_read_abs`
  so `audio_compare.py` and SDL playback are unaffected.

## Proof (measured — same-build before/after via `git stash`)
`audio_compare.py --rom roms/marios_tennis.vb`, deterministic (two 10 s runs
byte-identical):

| metric | baseline (point-sampled) | Blip | note |
|---|---|---|---|
| level offset @10 s | **+3.84 dB** | **+1.09 dB** | closed toward 0 ✓ |
| RMS (recomp) @10 s | 494.8 (67% of oracle) | **679.1 (92%)** | amplitude convergence ✓ |
| NCC @10 s | 0.0933 | 0.0905 | flat — tempo-drift-capped |
| NCC @2 s | 0.2305 | 0.2373 | short window ~2.6× the 10 s value |
| xcorr tempo drift | +3.6 ms/s | +10.8 ms/s | metric artifact (see below) |

**Outcome:** the predicted amplitude/spectral convergence landed (level
offset +3.84 → +1.09 dB, RMS 67 → 92% of oracle) — the output stage now
uses the oracle's exact gain chain. The hoped-for **NCC jump did NOT
materialize**: NCC is **capped by tempo drift**, not the output stage.
Evidence: short-window NCC (0.237 @2 s) is ~2.6× the 10 s value (0.091) —
waveshape correlates well locally but walks out of phase as drift
accumulates over the clip. The xcorr tempo number shifting (+3.6 → +10.8
ms/s) is NOT a real timing regression — `vsu.c` is CPU-timing-neutral by
construction, and the local-xcorr drift estimator responds to the changed
(now oracle-matched) waveshape; the independent onset-fit estimator
simultaneously reads −9.2 ms/s (opposite sign), i.e. both are
noise-dominated on this sparse-onset content. **The audio NCC ceiling is now
Axis-2/3 tempo drift, not Axis-5b.** No freeze (441000/441000 frames drained
both sides, 0 lost).

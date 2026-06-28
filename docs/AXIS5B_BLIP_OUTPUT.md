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
- [ ] `vsu.c` integration (this plan).

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
- **vsu_shadow coupling**: `vsu_emit_one_sample` feeds the `vsu_shadow`
  differential verifier the per-sample mix (`vsu.c:490-503`). Band-limited
  output has no per-sample integer mix to feed. The shadow QoL layer
  (opt-in, default OFF) is incompatible with Blip as-is — gate it off when
  Blip output is active, or rework it to compare post-Blip. Decide before
  removing the point-sample path.
- **Determinism**: `Blip_Synth_offset_resampled` is pure integer; volume is a
  one-time `double` at init. Must stay byte-identical across runs (verify).
- **Headless ring**: keep the same `s_ring` + `audio_pcm`/`vb_vsu_read_abs`
  so `audio_compare.py` and SDL playback are unaffected.

## Proof
`audio_compare.py`: NCC should jump from ~0.09 (point-sampled) toward the
envelope-correlation ceiling; level offset (~+3.8 dB) should close toward 0;
verify two-run determinism (identical RMS) and no freeze. Tempo/onset drift
(~3 ms/s) is a separate residual and should be ~unchanged.

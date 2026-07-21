# Shadow Audio + Screen Enhancements (Virtual Boy backport)

Backport of the gbarecomp / snesrecomp "verified-enhancement" QoL layer to
vbrecomp. The implemented layer lives in this core repository and remains
opt-in, default-off functionality.

## Governing principle (the carve-out)

Faithfulness is the product; these are an opt-in layer on top. The one
permitted form of HLE here is a **verified-enhancement shadow**, allowed only
when ALL hold (recomp-template `PRINCIPLES.md`, "Verified-Enhancement HLE Is
Allowed; Load-Bearing HLE Is Not"):

1. The emulated (canon) path keeps running and stays both the authoritative
   output and the verify oracle. The shadow is never ground truth.
2. The shadow is continuously, differentially checked against the canon stream
   and substitutes only after a proven window.
3. It reverts loudly (records a DEGRADE transition in the TCP-queryable shadow
   status ring) the instant it stops matching.
4. It is opt-in and present-time, off by default; with it off the output is
   byte-identical (frame bytes / audio bytes / vb-beetle compares stay on the
   raw canon).

Worst-case failure is "the user hears/sees the authentic hardware output," and
it cannot mask a recompiler bug because the canon path it shadows is still the
thing being diffed.

## Console specifics (Virtual Boy)

- **Video is RED-LED MONOCHROME.** Each pixel is a single red-intensity level
  (2bpp → BRT brightness cache → a 0..255 scalar → 1/2.2 gamma → red channel,
  G=B=0). There is **no RGB color gamut** — a GBA-style BGR555/CIE colorimetry
  LUT does not apply. The appropriate present-time model is a red-LED
  intensity/gamma curve, optionally with a small warm phosphor spill.
- **Audio is a 6-channel wavetable PSG** (`vsu.c`): 5 wave channels (32×6-bit
  tables) + 1 LFSR-noise channel, with per-channel envelope, ch4 sweep/FM
  modulation, and stereo level. The canon mix is integer: each channel's gain
  is requantized to `(envelope*level)>>3 + 1`, summed, scaled `<<2`, clipped to
  S16. The shadow re-renders the same channels in float with full-precision
  gain (no `>>3` requantize, no premature intermediate clip).

## What ports verbatim vs what is console-specific

| Piece | Status | Notes |
|---|---|---|
| **`ShadowVerifier`** (envelope-correlation self-check, auto-gain, prove/strike/pause) | **DONE** — `runtime/{src,include}/audio_shadow.{c,h}`, re-implemented in C, compiles + links clean | Engine-agnostic; identical algorithm to gbarecomp/snesrecomp. Ported verbatim, language-matched (C, like the VSU it shadows). |
| **VSU float shadow render + substitution** | **DONE** — `runtime/{src,include}/vsu_shadow.{c,h}` + `vsu.c` hook | VB-specific: re-renders the 6 VSU channels in float (full-precision gain) reading live channel state, drives the verifier, substitutes into the output ring only when `proven`; env-gated, DEGRADED on revert. |
| Present-path color LUT | **DONE (red-channel)** — `runtime/{src,include}/red_lut.{c,h}` + `vip.c` hook | VB-specific: NOT a BGR555/CIE gamut LUT. A 256-entry intensity→ARGB LUT keyed on the same 0..255 brightness scalar the canon renderer uses. RAW (default) is an exact passthrough. |
| GBA/SNES color-science core (xyY→XYZ, primaries, Bradford, sRGB OETF) | **N/A** | Does not apply — monochrome red display has no gamut to map. Documented, not ported. |

## Integration points

- **Canon audio render:** `runtime/src/vsu.c`
  - `vsu_channel_output()` (`vsu.c:265`) — canon per-channel integer render
    (`(envelope*level)>>3 + 1` gain).
  - `vsu_channel_output_shadow()` (`vsu.c:414`, NEW) — float mirror, same
    waveform/envelope/level selection, full-precision gain.
  - `vsu_emit_one_sample()` (`vsu.c:437`) — mixes both, scales `<<2`, then
    calls `vb_vsu_shadow_substitute(canon, shadow, &out)` (`vsu.c:473`) and
    stores the returned bytes into the stereo ring (`s_ring_l/r`). When the
    shadow is OFF the stored bytes are the canon bytes, byte-identical.
  - `vb_vsu_init()` calls `vb_vsu_shadow_reset()` (`vsu.c:120`).
  - SDL drains the ring via `vb_vsu_pull_samples` in the audio callback
    (`main.cpp:176`).
- **Audio shadow glue:** `runtime/src/vsu_shadow.c`
  - `vb_vsu_shadow_enabled()` — env gate `VBRECOMP_AUDIO_SHADOW` (default OFF).
  - `vb_vsu_shadow_substitute()` — drives `shadow_verifier_judge`, substitutes
    when `proven`, and records an `engage`/`degrade` transition into the
    always-on shadow status ring (`vb_vsu_shadow_get_status`) on each edge.
  - **No stderr/printf** (CLAUDE.md Rule 3): the prove/revert contract is
    surfaced over TCP. Query the runtime debug server (port 4390) with
    `{"cmd":"audio_shadow_state"}` — it returns `enabled`, `substituting`,
    `last_r`/`last_ratio`/`gain`, `engage_count`/`degrade_count`, and the recent
    transition ring (each `degrade` carries the verifier's revert `reason`).
- **Canon video present:** `runtime/src/vip.c`
  - `vb_vip_render_framebuffer()` (`vip.c:993`) — unpacks 2bpp → `bv` (0..255
    BRT cache) → `s_color_lut[bv]` (1/2.2 gamma) → red channel. The pack line
    (`vip.c:1028`) now routes the gamma-corrected red value through
    `vb_red_lut_map(r)`. RAW returns `0xFF000000 | (r<<16)` — byte-identical.
  - Called from the present path at `main.cpp:549` / `main.cpp:551`, then
    `SDL_UpdateTexture` (`main.cpp:555`).
- **Video LUT:** `runtime/src/red_lut.c` — env gate `VBRECOMP_SCREEN`
  (`raw` default / `led` / `led_warm`), lazy 256-entry table.
- **Build:** `runtime/runtime.cmake` (`_runtime_sources`) — added
  `audio_shadow.c`, `vsu_shadow.c`, `red_lut.c`. This list is reused by every
  game build (e.g. `MarioTennisVirtualBoyRecomp/`) via `vb_add_runtime_target`,
  so the layer reaches both `vb-runtime` and game binaries.

## Gating (default OFF ⇒ byte-identical)

- `VBRECOMP_AUDIO_SHADOW` — unset/`0` (default) OFF; `1`/`on`/`true` enables the
  VSU float shadow. When off, the verifier never runs and the stored audio
  bytes are the canon bytes.
- `VBRECOMP_SCREEN` — unset/`raw` (default) exact passthrough; `led` (pure
  red-LED, currently identity transfer pending a measured curve); `led_warm`
  (red-LED + small intensity-weighted warm spill into green).

## Out of scope (documented, NOT faked — never guess hardware)

- **LED persistence / motion smear.** The real scanned-mirror LED display has a
  slow-decay persistence producing characteristic motion smear. We do not
  synthesize a persistence/decay filter because we have not measured the decay
  constant; doing so would be guessing hardware behavior. Future: a measured
  temporal-decay present filter over consecutive framebuffers.
- **Stereoscopic dual-eye fusion.** The runtime already renders both eyes
  (`--stereo` stacks them); a true stereoscopic present (anaglyph / per-eye
  HMD / parallax) is a separate present-time feature, not modeled here.
- **A measured red-LED transfer curve.** The `led`/`led_warm` models use the
  existing 1/2.2 gamma red value as input; a colorimeter-measured red-LED
  luminance curve would replace the identity in `VB_SCREEN_LED` without
  disturbing the RAW byte-identity contract.

## Status / next steps

1. **Verifier** — DONE, compiles clean (C11 + C++17), links into `vb-runtime`.
2. **VSU float shadow** — DONE, wired + builds; default-OFF path is
   byte-identical (verifier not invoked when disabled). NEXT: A/B against
   vb-beetle on Mario's Tennis with `VBRECOMP_AUDIO_SHADOW=1` to confirm the
   verifier proves on real engine output and that the requantize-free mix is
   audibly cleaner; tune `prove_need`/strike thresholds if the VSU's pacing
   differs from the GBA FIFO assumptions.
3. **Red-LED LUT** — DONE (RAW passthrough byte-identical; `led_warm` opt-in).
   NEXT: source a measured red-LED transfer curve for `VB_SCREEN_LED`; consider
   a persistence model once the decay constant is measured.
4. Verify default-off is byte-identical on a full game build (Mario's Tennis),
   then A/B the enhancements.

## Compile / build status

- `audio_shadow.c`, `vsu_shadow.c`, `red_lut.c`, modified `vsu.c`, modified
  `vip.c`: compile clean standalone with `gcc -std=c11 -Wall -Wextra`.
- Full `vb-runtime` (no-game stub) configures and links clean with the new
  files wired into `runtime.cmake` (Ninja + MinGW gcc 15.2, MSYS2 SDL2).
- `vb-beetle` oracle target was not built here (no static archive present in
  this worktree; see `docs/BRINGUP.md`).

## Attribution

`ShadowVerifier` ported from JRickey/gba-recomp (`crates/gba-core/src/shadow.rs`)
via the gbarecomp C++ and snesrecomp C ports, © Jrickey, MIT OR Apache-2.0,
used with permission. The VSU float re-render and the red-LED present LUT are
original to this project. See `THIRD_PARTY_ATTRIBUTION.md`.

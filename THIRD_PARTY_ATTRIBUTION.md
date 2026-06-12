# Third-Party Attribution

Portions of this project are ported from other open-source projects, used with
permission and under their licenses. Ported logic is credited below; each
ported file also carries an attribution header pointing here.

## JRickey/gba-recomp

- **Upstream:** https://github.com/JRickey/gba-recomp
- **Author:** Jrickey
- **License:** MIT OR Apache-2.0 (used with the author's permission)

| Our file | Upstream source | What was ported |
|---|---|---|
| `runtime/src/audio_shadow.{c,h}` | `crates/gba-core/src/shadow.rs` (via the gbarecomp C++ port `src/gba/audio_shadow.*` and the snesrecomp C port `runner/src/snes/audio_shadow.*`) | Engine-agnostic verified-enhancement HLE-shadow differential verifier: envelope-correlation self-check vs the canon stream, probation auto-gain calibration, prove/strike/pause-and-reprobe. Re-implemented in C. (Permitted under PRINCIPLES.md "Verified-Enhancement HLE Is Allowed; Load-Bearing HLE Is Not".) |

### Original to this project (structure inspired by, not ported from, the above)

- `runtime/src/vsu_shadow.{c,h}` — the float re-render of the VSU 6-channel
  wavetable mix and its substitution/gating glue. The VSU is Virtual Boy
  hardware; the float re-render reads the live channel state and is original.
- `runtime/src/red_lut.{c,h}` — present-time red-LED intensity→RGB LUT. The
  module structure mirrors the gbarecomp present-time color LUT
  (`src/runtime/color_lut.*`, itself ported from JRickey/gba-recomp
  `crates/screen`), but the GBA BGR555/CIE-gamut color science does **not**
  apply to the Virtual Boy's monochrome red display; the red-LED intensity
  model and the single-channel apply path are original to this project.

Ported code remains under the upstream's MIT OR Apache-2.0 terms; this notice
and the per-file headers satisfy the attribution requirement.

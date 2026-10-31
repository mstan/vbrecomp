# Native VIP GPU raster presentation

`VBRECOMP_GPU_RASTER` builds an optional owned OpenGL3.2 context backend.
`--internal-resolution 1..64` and the standard internal-resolution setting
select density independently of window size. Unsupported windows/capabilities
and unqualified custom renderers keep the original portable presentation.
Initialization failure is latched until reset/window replacement. Requested and
effective density are reported once when changed; the hardware limit can lower
it. Native1x is the default and still requires a per-game neutrality gate.

The guest VIP timing, original packed framebuffers, reads and state visitors
remain native. Immutable eight-row draw captures feed ordered normal, HBias,
affine and OBJ jobs. CPU stores to a framebuffer mark the affected eight-row
band as original native bitmap; enhanced geometry is never inferred from it.
A portable consumer of the same job descriptions provides native reference
pixels. Affine horizontal subpixel sampling uses the original row parameters;
there is no invented vertical interpolation between native parameter rows.

One fixed12x7 logical32x32 grid per eye renders through a single32*S GPU tile,
at most2048x2048. Host jobs, source uploads and tile submission count do not grow
with S. Resident integer DRAM/CHR pages reuse existing capture revision tokens;
zero/absent tokens require normal uploads. Colors/palettes and original bitmap
changes are uploaded separately. No enhanced CPU framebuffer or SDL streaming
backing is allocated. The backend owns its context and restores the caller's
context, including no-current-context; it does not mutate SDL's private cache.
GPU-only captures are excluded from public/history state visitors and discarded
on load/reset. Native bitmap fallback remains until new VIP draws recapture.

Arbitrary game video, viewport/HUD policies, color callbacks, override overlays,
linear filtering, state UI and open/toast runtime UI currently use the unchanged
CPU presentation. This backend therefore qualifies the stock VIP slice only;
those fallbacks do not establish16x rendering for modded workloads.

The existing finite benchmark JSON reports actual GPU/fallback frames, requested
and effective scale, initialization failures, actual API upload bytes and optional
asynchronous ARB_timer_query timestamp totals. Four query pairs are bounded;
unavailable/pending/skipped samples remain explicit and never force a GPU wait.
Timing covers upload/raster/blit commands before swap, not swap/vsync waiting.
The hidden `vb-vip-gpu-raster-test` is explicitly run under a hardware lease;
it compares real native GPU pixels to the portable and existing native renderer,
including both eyes and native bitmaps, and exercises16/32/64x bounded tiles.
Component qualification alone is not game performance or acceptance.

Timer contract: https://registry.khronos.org/OpenGL/extensions/ARB/ARB_timer_query.txt

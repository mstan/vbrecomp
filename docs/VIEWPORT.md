# Opt-in presentation viewport

`runtime/include/viewport.h` exposes a wider, read-only replay of captured VIP
drawing inputs. This is a presentation renderer; CPU dispatch, device timing,
VRAM, SRAM, packed native framebuffers and native screenshot APIs stay unchanged.
The first consumer is SD Gundam: Dimension War's optional widescreen package.

## Contract and lifetime

Call `vb_viewport_track()` during game startup to capture inputs even while the
feature is disabled. Each native eight-row drawing block copies DRAM, CHR, SPT,
decoded palettes, backdrop and object suppression into its drawing framebuffer
slot. Two sets of 28 snapshots follow VIP display/drawing ownership, using about
10 MiB including optional title metadata. Initialization invalidates them. Mod selection resets preserve them so
enabling a feature while paused can render the already displayed frame.

Activate one provider with `vb_viewport_register()`. The host's mod system uses
the separate exclusive `video.viewport` resource; `video.renderer` remains
available for color composition. Registration rejects invalid or duplicate
providers. A title's classifier returns zero for unsupported scenes, keeping
the native 384x224 presentation. All 28 displayed snapshots must be available.

Callbacks receive borrowed, read-only world, object and CHR tables. An optional
layout moves selected world pixels within a specified vertical interval using
left/center/right anchors, and shifts selected OAM base x coordinates. Eye
parallax remains in the shared rasterizer. The title owns scene recognition and
layout policy. It must establish that new bounds expose valid source content.

`vb_viewport_track_extra()` registers one startup capture callback for at most
16 KiB of title-owned metadata per block. This data follows the same framebuffer
lifetime as VIP input; presentation callbacks must not substitute newer live
guest state. The callback writes only the supplied host buffer.

A normal BG world's `outside` callback can map samples beyond its horizontal
bounds back to original source-map coordinates, preserving CHR provenance.
Up to 96 extra sprites may reference borrowed original cell maps and CHR banks.
Each supplies an eye, native-size position and a world after which it is drawn,
so subsequent native worlds still occlude it. Their source records use OBJ
kind, map 255 and the selected world's identity, including CHR hash and flips.
Pointers must remain valid for the complete replay call. These hooks are opt-in
title policy; they do not create objects from arbitrary guesses.

`vb_viewport_render()` replays BG, HBias, affine and OBJ addressing from those
snapshots, including source attribution, palettes, flips and eye selection.
It accepts even widths from 384 to 2048 at a fixed height of 224. Width 384
bypasses title transforms for direct native-raster comparison. Wider output
removes horizontal screen clipping while respecting each submitted world's
bounds unless an explicit outside mapping is supplied. Recovery of entities
culled before VIP submission requires title-owned captured records and original
art through the extra-sprite API. The shared renderer does not discover those
records or extend camera movement.

This replay models VIP drawing inputs. It does **not** replay direct CPU writes
to the packed framebuffer. A title that uses those stores in a widened scene
needs additional provenance-aware presentation work before enabling this path.
The native-width replay comparison is a required gate for such scenes; copying
the native center over a mismatching replay would conceal the defect.

## Presentation

The host supplies output width and per-eye height to `vb_viewport_window()`.
Adaptive mode uses their ratio; fixed mode uses the registered fraction.
Width is rounded to the nearest even integer so the native image stays centered
on its original pixel grid. Narrow windows preserve 384 pixels; extremely wide
ones cap at 2048. SDL scales uniformly and letterboxes any residual mismatch.
Stacked stereo divides window height between the eyes before deriving aspect.

`vb_renderer_present_width()` reports the current extent.
`vb_renderer_present_viewport()` renders that extent and then invokes the color
callback with `VbRenderFrame.width/height`. Color providers used with this API
must iterate those dimensions, rather than `VB_RENDER_PIXELS`. All arrays are
row-major and valid only for the callback. `vb_renderer_present_sources()`
returns the last wide replay's source array, valid until the next presentation;
call presentation for the desired eye immediately before reading it.

The original `vb_renderer_present()` stays fixed at 384x224. Legacy overlay
composition in the SDL host operates on the centered native region. No custom
policy is enabled in other games merely by linking the shared runtime.

## Change manifest and proof

The implementation consists of `viewport.c/.h`, a capture observer in `vip.c`,
dimension-aware presentation/color interfaces, SDL texture resizing, TCP
inspection, and `--set-mod-option package:feature:option=value` in the host.
There are no changes to native raster loops, guest code or hardware rules.

`vip-source-test` verifies BG/HBias/affine/OBJ replay and source identity across
32 mode/flip/affine combinations and both eyes, native-center equivalence at
796 pixels, actual clipped edge recovery, snapshot lifetime and aspect limits.
It also checks extra metadata lifetime, duplicate registration rejection,
out-of-bounds BG source mapping and flipped extra sprites in both eyes.
The complete runtime CTest suite passes 6/6.

Title-level evidence is in SD Gundam's `docs/WIDESCREEN.md` and compact JSON
reports. Its validator compares independent original/wide native processes
across nine guest/raw/audio planes, checks both-eye 384-wide replay against
the original raster, and validates wide source attribution and color output.
Real SDL tests exercise live resizing, stacked eyes, pause/resume and battery
save publication. These tests establish the captured title routes, not universal
coverage of all games or every VIP edge case.

Central framework issue: `beads-vb.1.6`; title issue: `beads-vb.4.4`.

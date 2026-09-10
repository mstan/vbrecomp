# Virtual Boy mod packages and custom renderers

Mods are ZIP archives with the `.vbmod` extension. The runtime reads the original
ROM, verifies its exact SHA-256, and activates selected host-side plugins. It never
patches or rewrites the ROM file or generated C. Native plugin implementations
are linked by the game's developer; an installed archive cannot load a DLL or
introduce executable guest code.

The package catalog and shared recomp-ui provider follow snesrecomp's feature
model: independent toggles and typed options, installed versions, conflict
diagnostics, and saved selections. The adapted package manager retains its
PolyForm Noncommercial license in `runtime/licenses/snes-mod-runtime.txt`.
The standalone SHA-256 helper is public domain; the CRC helper is from the MIT
recomp-ui project. Other framework files retain their existing licenses.

## Install and configure

The launcher **Mods** view installs/removes `.vbmod` archives, selects installed
versions, enables features, and edits options. The in-game settings menu exposes
the installed feature toggles. Conflicting or incompatible features cannot commit.

Default layout, relative to the executable:

```
mods/
  state.toml
  packages/<package-id>/<version>/manifest.toml
  packages/<package-id>/<version>/<assets>
vbrecomp.cfg
```

Use `--mods-dir PATH` and `--config PATH` for separate profiles. Headless use:

```
MarioTennisVirtualBoyRecomp --rom original.vb --headless --install-mod color.vbmod --enable-mod color:color
```

`--disable-mod package:feature` reverses a feature selection. `--no-launcher`
skips the launcher for that run; `--launcher` forces it even when the saved
skip-launcher setting is enabled. ROM verification still runs in both cases.

## Author a package

Put package metadata at the TOML root, followed by array sections. This is the
same constrained declarative TOML vocabulary as the SNES runtime (one assignment
per line; no multiline strings or inline tables):

```toml
format_version = 1
id = "color"
version = "1.0.0"
name = "Full color experiment"
author = "Your name"
license = "MIT"

[[target]]
game_id = "marios-tennis"
rom_sha256 = "5dc5e6b5d5f538f56b3b9727db1c7931d9dfe1bbd0743f698897e0fd90e70101"

[[feature]]
id = "color"
name = "Full color"
description = "Experimental game-owned color renderer."
group = "Graphics"
channel = "experimental"
default_enabled = false

[[plugin]]
feature = "color"
id = "marios-tennis.full-color"

[[option]]
feature = "color"
id = "saturation"
label = "Color strength"
type = "integer"
default = 100
min = 0
max = 100
step = 5
```

Option types are `boolean`, `integer`, and `choice`. A choice adds
`[[option.choice]]` entries with string `value` and `label` fields. The plugin
defines which options it consumes; metadata alone does not implement behavior.
Each enabled feature must target the verified game. Two enabled features cannot
claim the same plugin ID or exclusive resource. Missing registered plugins
produce a diagnostic. Feature channels are `stable`, `experimental`, or `developer`.

Build an archive using `python tools/pack_mod.py path/to/source output.vbmod`.
No ROM is needed to author/package a mod. Installation validates paths and CRCs,
supports stored and deflated ZIP entries, and limits expansion to 256 MiB/4096
files. Installed version directories are immutable: publish a new version for
an update. Disabling features is required before removing their active version.

## Implement a trusted plugin

Game code registers an activation function with
`vb_mod_register_activation_plugin`. Use `VB_MOD_CONSTRUCTOR` for portable
static registration. Register any cleanup through `vb_mod_register_reset_callback`.
During activation, `vb_mod_option` copies committed option values and
`vb_mod_asset` resolves an existing file contained inside that package's root.
Staged edits do not change the committed plugin inputs.

Renderer implementations register with `vb_mod_register_exclusive_plugin` using
the shared resource `video.renderer`, and call `vb_renderer_track_worlds()` at
startup. That keeps attribution available even if the first activation happens
while the game is paused. Resetting a renderer does not turn off that tracking.

Optional `[[resource]]` entries describe explicitly selected owner files/folders:
`feature`, `id`, `label`, `format` (`file` or `directory`), `required`, and optional
`file_patterns`, `file_description`, `size`, and exact-file `sha256`. A selected
resource is validated before commit. Directory resources cannot declare a file
hash. `vb_mod_resource` copies its committed path during activation. Package
assets use `vb_mod_asset` instead and cannot escape the installed package root.

Frame callbacks registered during activation run at the native VIP GAME_START
boundary. Presentation callbacks run through `renderer.h` and receive a
`VbRenderFrame`: displayed frame sequence, eye, stock ARGB, unpacked native 2bpp
levels, and a per-pixel world index. World attribution is 0 for backdrop and
1..32 for world 0..31; it follows the native double buffers and occlusion order.
The host owns the output buffer. Callbacks are synchronous on the runtime thread
and must not retain borrowed pointers. Only one renderer can be active.

Artwork-aware renderers can call `vb_renderer_track_texels()` at startup (this
also enables world tracking). `frame->sources` then provides a `VbSourceTexel`
for each displayed pixel: original CHR content hash, unflipped tile `u/v`, raw
2bpp value, BG-map `x/y`, world index, map base, and draw mode. Modes are normal
BG (0), horizontal bias (1), affine (2), and OBJ (3, map 255). `world == 0`
means no VIP world source. The rasterizer records this alongside its actual
sampling and occlusion, in the same double-buffer slot as the native pixels.
Changing CHR RAM for the next animation does not change the displayed metadata.
This avoids guessing character identity from world numbers or reading mutable
VRAM at presentation time. Only the game module interprets artwork or materials.

CPU framebuffer drawing uses source kind **4**, map **255**, world **0**. For
that kind, `tile_hash` holds an opaque game-owned presentation tag (0 means
unclassified), `x/y` are framebuffer coordinates and `raw` is the written 2bpp
pixel. CHR fields do not apply. The `VBSRC001` record stays 16 bytes; consumers
must distinguish kind 4 before interpreting the first word as a CHR hash.

A trusted game can register one `VbWriteObserver` through
`vb_memory_register_write_observer`. It sees a read-only CPU pointer and aligned
store address/value/width before each bus write, for both generated and
interpreter execution. It may update host-only provenance and return a tag for
direct framebuffer writes. It must not mutate guest state or call the bus.
Registration persists across guest resets; game-specific metadata must tolerate
that lifetime. No package can introduce this code dynamically.

Only changed 2bpp pixels acquire the returned tag. Packed read/modify/write stores
retain unchanged pixels' owners, zero pixels lose their source, and native VIP
drawing replaces the affected source metadata normally. Eye and framebuffer slot
follow the actual address, including VIP mirrors. Attribution is available even
when the color feature is disabled, allowing later activation while paused.

The framework resets the renderer before activating the next committed plan.
Use the game's own module to interpret world identities and game state; generic
VIP rasterization must remain game-independent. The raw VIP renderer remains
available for oracle comparisons.

## Verification

`--paused` starts before the first guest instruction. TCP `run_frames` with
`frames: N` advances the runtime frame counter by N and then pauses. Poll
`get_registers` until its `frame` field reaches the returned target. This counter follows
emulated CPU cycles; it is distinct from VIP drawing-frame sequence numbers.

TCP `screenshot` with `presented: 1` uses the custom renderer. Without that flag
it captures the stock renderer. Both accept `eye: 0` or `eye: 1`. Runtime UI is
not part of these screenshots; use `host: 1` to capture the window's presentation
buffer including the menu. Compare the same deterministic input route with
mods enabled/disabled, including WRAM and both raw eye images; inspect the
presented images separately. `world_map` reports the displayed attribution.

The existing development TCP server listens on loopback, default port 4390;
`--port` selects an isolated test instance. Pause before taking several related
captures. For example, send one newline-terminated JSON request per connection:

```json
{"cmd":"pause"}
{"cmd":"screenshot","path":"F:/captures/color.png","presented":1,"eye":0}
{"cmd":"source_dump","path":"F:/captures/color.src","eye":0}
{"cmd":"continue"}
```

`source_dump` requires startup texel tracking. Its portable little-endian file
contains magic `VBSRC001`, four `uint32` values (width, height, eye, current VIP
frame sequence), followed by width*height 16-byte records in screen row order.
Python record format is `<IHHHBBBBBB`: hash, x, y, tile, u, v, map, kind, raw,
world. This snapshot and `screenshot` execute synchronously without advancing
the guest. Production builds with `VBRECOMP_DEBUG_TOOLS=OFF` open no TCP port.

Build and run the ROM-free package integration test with CTest (`vb-mod-runtime`).
It covers package extraction, identity, feature conflicts, options, persistence,
activation/deactivation, and original ROM preservation.
`vb-vip-source` additionally checks synthetic CHR through all four native drawing
modes, both eyes, flips, transparent occlusion, unchanged native output, and
displayed metadata remaining stable after CHR RAM and the drawing buffer change.

Legacy `VBRECOMP_OVERRIDES` and related capture/recolor experiments still exist
for prior workflows. Clear them when testing package renderers; they can otherwise
add their own native draw suppression or overlays. `.vbmod` installation does not
set those environment variables.

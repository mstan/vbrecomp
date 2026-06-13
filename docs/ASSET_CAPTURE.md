# Runtime graphics capture + colored OBJ replacement (experiment)

A small, opt-in layer that (1) **captures** the graphics Mario's Tennis actually
decodes at runtime, identified by **content hash** rather than mutable VRAM
address, (2) **reconstructs** larger composites from the 8×8 tiles, and (3)
**replaces** a captured OBJ tile with a colored RGBA image that tracks the
original's position, flip, priority, and per-eye placement — while staying
**byte-for-byte faithful by default**.

It follows the cross-project extensibility contract (snesrecomp `overrides/`,
SMW widescreen): faithfulness is the product; this is an opt-in layer on top.
With the env switches unset the output is byte-identical and oracle compares are
unaffected — proven below.

---

## 1. The VIP render path (where we hook)

`runtime/src/vip.c` is a faithful port of Beetle VB's `vip_draw.inc`. A
column/block state machine draws **28 eight-row blocks per frame** into a 2bpp
**back** framebuffer by walking **32 world descriptors** (world 31→0), then flips
it to the displayed buffer. Left/right eyes are drawn separately with per-object
parallax. World kinds:

- **OBJ** (`BGM_OBJ`): hardware objects/sprites from OAM (`draw_obj`, vip.c:816).
  Per entry: `char_no`, the fetched 8×8 CHR pixels, per-eye X (`jx ± jp`), Y
  (`jy`), hflip/vflip, palette, per-eye visibility (`jlron[lr] && lron[lr]`).
- **BG / affine** (`draw_bg` / `draw_affine`): scrolled/perspective background
  layers built from a cell map.

**Important Mario's Tennis finding:** the game uses hardware **OBJ almost
exclusively for the in-match HUD/scoreboard** (and small icons). The
title logo, character-select portraits, and the on-court *players/ball* are all
drawn as **BG/affine** (the players are perspective-scaled affine layers). So the
OBJ-replacement proof targets the scoreboard text; the on-court player is a
documented next step (affine-aware replacement — see §8).

The framebuffer is **2bpp red-LED monochrome** — there is no RGB in the native
pipeline — so a full-color RGBA replacement cannot live in the 2bpp buffer. It is
a **per-eye overlay composited at present time**, after the faithful render.

## 2. Capture point selected (and why)

The capture/resolve passes run at **drawing-frame start (GAME_START, vip.c
inside the `XP_EN` guard)**, reading the same world/OAM/CHR DRAM the
about-to-draw blocks will read — one coherent snapshot per frame. (XP_END would
also be coherent but tags the *back* buffer; GAME_START is cleaner and is where
the double-buffer flip happens.) The pass mirrors the renderer's table walk
exactly (world stride `0x1D800 + world*0x20`, OAM `0x1E000 + oam*8`, OBJ groups
via `SPT[]` / `obj_search_which`) so priority/group attribution matches.

This is the **ring-buffer model**: when enabled, capture runs continuously and
records every draw-use into rings; the dump just **queries** the rings — it never
arms-then-runs.

## 3. Implementation

Generic engine (framework, never names a game), all gated, default OFF:

| File | Role |
|---|---|
| `runtime/src/vip_capture.{c,h}` | capture rings: deduped tile catalog (by content hash) + draw-use ring + frame layouts; PNG/JSON dump. Shared `vb_capture_hash()`. |
| `runtime/src/asset_pack.{c,h}` | override pack loader (manifest + RGBA images), content-hash lookup, double-buffered per-eye overlay draw-list + `vb_overlay_composite()`. |
| `runtime/src/png_read.{c,h}` | self-contained stored-block RGBA PNG decoder (sibling of `png_write.c`; no zlib dependency). |
| `runtime/src/vip.c` | capture pass + resolve pass (suppress-set + overlay list) at GAME_START; per-OAM suppression in `draw_obj`; per-game-frame `vb_input_frame_advance()`. |
| `runtime/src/main.cpp` | present-time overlay composite, per eye, after `vb_vip_render_framebuffer`. |
| `runtime/src/debug_server.c` | TCP commands `capture_dump`, `overrides_state`, `press`, and `screenshot {overlay:1}`. |
| `runtime/src/input.{c,h}` | frame-counted `vb_input_press()` for deterministic headless navigation. |

Game-specific data lives in the **game repo** under `overrides/graphics/`
(manifest + colored PNGs) + `tools/` (compose/colorize). Nothing game-specific
is in the framework.

### Hashing policy

Content hash = **FNV-1a over the 16 raw 2bpp bytes of the UNFLIPPED 8×8 tile**.
This is independent of VRAM address, frame, OAM index, and screen position. Flips
are **recorded separately per draw-use** (not normalized into the hash). The same
slot holding different graphics → different hash; the same graphics in multiple
slots → one catalog entry.

## 4. Switches (faithful by default)

| Env var | Effect | Default |
|---|---|---|
| `VBRECOMP_CAPTURE` | allocate rings + run the capture pass; `capture_dump` writes files | unset = off, no allocation, no per-frame work |
| `VBRECOMP_OVERRIDES=<dir>` | load `<dir>/graphics/manifest.json` + images; resolve + suppress + overlay | unset = off, faithful |

With both unset the renderer, `vb_vip_render_framebuffer`, and the present path
run exactly as before. A missing/empty/malformed manifest or a missing image
disables only the offending entry and **never aborts** (mirrors `red_lut`'s
"unrecognized token → stay RAW").

## 5. TCP command surface (port 4390)

- `capture_dump {dir}` → writes `captures/` (session.json, tiles/<hash>.png +
  <hash>@8x.png + <hash>.json, layouts/frame_NNNNNN.json). Deduped tiles; metadata
  preserves every use. No-op if capture inactive.
- `overrides_state` → `{active, images, display_slot, overlay0, overlay1}`.
- `press {buttons:<VB_PAD_* mask>, frames:N}` → hold N **game-frames** then
  auto-release (deterministic headless navigation, independent of wall-clock).
- `screenshot {path, eye, overlay:0|1}` → `overlay:1` composites the override
  result; default (no `overlay`) is the faithful raw render (keeps oracle
  compares byte-identical).

## 6. Capture output (Mario's Tennis)

Example, in-match HUD frame: **63 unique deduped OBJ tiles** in the catalog, with
per-tile use metadata and per-frame layouts. The composite tool
(`MarioTennisVirtualBoyRecomp/tools/compose_capture.py`) reassembles the OBJ
tiles of a frame at their recorded positions into a recognizable sheet — e.g. the
scoreboard reconstructs to `SET 1 / MARIO 0 / DONKEY 1`. (Individual 8×8 tiles
are not recognizable; the composite is — exactly why reconstruction matters.)

## 7. The replacement proof

`tools/colorize_tiles.py` recolors the captured "MARIO" scoreboard tiles into a
Mario red/yellow palette and writes `overrides/graphics/<hash>.png` +
`manifest.json`. With `VBRECOMP_OVERRIDES` set, in a live rally the scoreboard
renders **`MARIO 0 / DONKEY 1` in color** while the court, player, and other HUD
stay faithful monochrome (`docs/proof/replacement_before_after_eye0.png`).
Verified:

- **Both eyes** render the overlay (`docs/proof/replacement_stereo_pair.png`);
  per-eye visibility + per-eye X honored (HUD parallax is 0, correct for a HUD).
- **Flip** works — the font's mirrored glyphs (e.g. M/A/O drawn hflipped) render
  correctly.
- **Position/priority** tracked — overlays sit exactly on the suppressed
  originals as the score updates.
- **Faithful fallback** — with the env unset, the boot frame is **byte-identical**
  to a pre-change baseline (`cmp` clean, both eyes); `overrides_state` reports
  inactive.
- **Graceful config** — a deliberately malformed manifest loads 0 images, the
  cart keeps running, no abort.
- **Shared-tile / false-match behavior (observed & expected):** content-hash
  matching colorizes a glyph **everywhere it appears**, so "DONKEY" also colorizes
  (it shares font tiles with "MARIO"). This is correct for per-tile replacement;
  isolating a single word needs cluster matching (§8).

## 8. Recommendation for the next iteration

1. **Cluster / metasprite recognition** — match a specific arrangement
   (`cluster_hash` over sorted `(rel_x, rel_y, tile_hash, flip, palette)`, already
   emitted by `compose_capture.py --clusters`) and replace it with one image, so a
   shared glyph isn't colorized globally. This is the cleanest fix to the
   shared-tile effect and the path to replacing a word/logo as a unit.
2. **Affine/BG replacement (the on-court player)** — the visually-rich Mario is an
   affine layer, not OBJ. Extend the resolve pass to affine worlds and composite
   an **affine-transformed** RGBA overlay (the engine — pack, hashing, per-eye
   composite, faithful contract — already generalizes; the delta is the transform
   math and coarser per-world suppression).
3. **Arbitrary-PNG ingest** — `png_read.c` currently parses stored-block PNGs
   (what our tools emit). Add a minimal inflate (or link zlib) to ingest
   AI-generated/compressed PNGs directly.
4. **Deterministic recoloring** — derive replacement palettes from the captured
   2bpp values programmatically for whole tile classes (HUD font, etc.).

## 9. Present-time recolor (scene-aware world packs)

A second opt-in layer recolors the **whole screen** at present time instead of
overlaying RGBA sprites. The VIP rasterizes every world into a 2bpp brightness;
`vip.c` tags each rasterized pixel with the **world index** that drew it (only
when capture or recolor is active — otherwise no attribution is written and the
faithful path is byte-identical). At present,
`vb_vip_render_framebuffer_recolored` maps (world, vertical band, brightness)→RGB
using `recolor.{c,h}` and the pack at `overrides/recolor/palette.json`. Keying on
the world index (not tile content) makes a character stay colored through its
whole animation.

**Pack format** — scene-aware:

```json
{ "scenes": [
  { "name": "match_luigi",
    "detect": { "all": [22], "none": [29], "ram": {"addr":"0x0500203A","eq":1} },
    "worlds": [
      { "world": 28, "label": "court", "ramp": ["#000","#0a4018","#1c7a30","#34c050"] },
      { "world": 22, "label": "luigi", "bands": [
          { "hi": 60, "ramp": ["#000","#0a5a18","#18a030","#30d048"] },
          { "hi": 256, "ramp": [ ... ] } ] }
    ] }
] }
```

- A world index means different things in different scenes, so rules are grouped
  per **scene**. Each frame the runtime builds the bitmask of worlds present
  (≥16px) and `vb_recolor_select_scene` picks the **first** scene whose `detect`
  matches: `(mask & all)==all`, `(mask & none)==0`, and — if present — a `ram`
  byte equals `eq`. An empty `detect` matches always (order it last as a
  fallback). No match ⇒ that frame renders faithfully.
- The `ram` predicate gates a scene on a game RAM byte read via `vb_memory_dump`.
  It exists so one on-screen layout (e.g. the near tennis player, always world
  22) can resolve to different rules per selected character. **The address is
  game-specific and lives only in the pack — never in the generic runtime.**
- A world rule is either `"ramp"` (flat, 4 colors indexed by brightness) or
  `"bands"` (vertical `hi` cutoffs in 0..256 of the world's on-screen bbox, each
  with its own ramp) for per-part body coloring. Legacy flat `{"worlds":[...]}`
  packs still load as one always-matching scene.

**TCP:** `recolor_state` (active / entries / scenes / current scene name),
`recolor_reload` (re-read the pack live), `world_map {eye}` (per-world bbox+count,
for identifying worlds when authoring), `screenshot {recolor:1}`.

**Mario's Tennis packs** (in the game repo): title, mode_select, and per-character
match scenes for all 7 characters, gated on `0x0500203A` (P1 character index
0..6). Generated by `tools/make_match_palette.py`; bootstrap a scene from a live
`world_map` with `tools/build_palette.py`.

## Attribution / discipline

No generated code is touched. PNG I/O is self-contained (no zlib), matching the
project's existing `png_write.c` choice. The faithful-default contract mirrors the
shadow-enhancement layer (`docs/SHADOW_ENHANCEMENTS.md`) and snesrecomp
`overrides/`.

# Shared ready-row shading

`VBRECOMP_STREAM_SHADING` defaults on. The runtime streams only explicitly
qualified providers. `vb_renderer_register_local_pixels` promises independent
per-pixel output using current stock/level/source/world inputs and stable provider
data. Callbacks must accept dense batches with height1, independent of absolute
output extent; input pointers are borrowed/read-only for that invocation.
Frame sequence, eye and original artwork metadata remain available. Transparent
memoization is allowed; semantic per-invocation effects and full-frame inspection
are not compatible with this additional capability.

Viewport streaming also requires `vb_viewport_register_row_local`: policy and
outside callbacks observe scene/policy inputs, never partially produced output.
These are independent declarations. Missing either declaration retains the old
full-image path. Existing registrations keep their unchanged full-stock contract,
including arbitrary stock reads after output writes. Reset and failed duplicate
registrations do not transfer capabilities between providers.

The shared viewport rasterizer clears, paints, resolves brightness and shades
each completed eight-row region while its metadata is nearby in cache. It keeps
complete source/level/world arrays for existing readers. Native rendering uses
bounded row scratch rather than a second complete stock image for qualified
callbacks. No guest drawing, capture timing, source identity or color policy is
changed. The disabled control retains the same APIs with full-frame dispatch;
exact-alias apply operations also retain independent read-only callback stock.

Wario's consumer integration changes two registration calls. Its material/color
and layout algorithms are unchanged. Zero's full-frame HUD analysis does not
qualify and uses its existing registration and path. No title-specific algorithm,
cache, geometry or transfer rule is introduced.

Contract tests exercise legacy arbitrary reads, local batches, independently
qualified viewport providers, aliases, resets, duplicate registration and the
disabled control. The independent scalar raster comparison includes386 cases
with both eyes, maximum width, changed captures, flips, wrap, affine/HBias and
outside callbacks; color and every metadata plane must match. Built-game
behavior/performance remain required before adoption.

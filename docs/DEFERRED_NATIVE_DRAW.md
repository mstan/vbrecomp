# Native drawing from immutable captures

Native drawing events retain their exact guest cycle and IRQ schedule. When
viewport capture is already enabled, the native framebuffer may reuse the
immutable input bank captured at that event and materialize its completed rows
when observed. Expanded presentation uses the same captured input. Other games
retain eager native drawing.

A complete native drawing event replaces all384 pixels of an eight-row block
for both eyes. An unobserved older event for the same framebuffer slot/block can
therefore be discarded when the replacement completes. Guest framebuffer reads,
partial stores, write observers, raw/native presentation, attribution/source
access and state serialization materialize all required preceding rows first.
No guest cycles advance during materialization. Native SRAM and device timing
are unaffected.

One spare capture bank keeps the previous immutable input alive while a row's
metadata callback runs. That callback may observe the previous native result.
After it returns, the replacement owns the new bank. DRAM/CHR are copied only
once, as in the existing viewport capture; only metadata needs preservation when
an old bank is borrowed. Rows without outstanding native work update in place.
All arrays remain contiguous for existing scene callbacks. State visitation
keeps its original row order and bytes; host pointers and pending jobs are not
serialized. State loads discard pending jobs after materialized state restores.

Drawing helpers consume an explicit read-only input context, including captured
palettes, object suppression, backdrop and attribution/texel flags. They use
neither newer live artwork nor later renderer settings. Their scratch remains
guest-thread owned, as before. Capture entry points remain VIP-owned; recursive
capture from a capture callback is not supported.

`VBRECOMP_DEFER_NATIVE_DRAW=OFF` retains eager execution for compiler controls.
The selection is part of strict build identity. Both paths are portable C11.
Runtime output records scheduled, materialized and overwritten native blocks;
avoided work is diagnostic evidence, not a substitute for matched built-game
CPU measurements and both-eye/device/audio/state qualification.

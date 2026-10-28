# Portable packed-framebuffer decoding

P8 evaluates burndown VB28 using a portable bulk kernel. No CPU instruction-set
extension, assembly, GPU backend or guest scheduling change is required.

One framebuffer byte contains four consecutive vertical pixels. The kernel
loads it once and writes all four decoded rows, retaining the exact current
column-brightness/LED palette and optional raw-level plane. Existing native-row
materialization happens before decoding. Both eyes, slots and all output pixels
keep their original values. If ARGB and level output buffers overlap, the old
scalar traversal preserves their original write order.

`VBRECOMP_FRAMEBUFFER_BLOCKS=OFF` retains the original scalar loop under the
same interface. The existing eager fixture explicitly compiles that control;
the deferred fixture compiles the block kernel. Independent per-pixel expected
values cover column brightness, LED modes, both slots/eyes and the fused plane.
Built-game per-game measurements remain necessary before acceptance.

The selection evidence is a retained post-P5 Zero profile with245/3817 leaf
wall samples in framebuffer decoding, and Wario142/6374. Unchanged instruction
and relocation comparisons qualify the inlined attribution. Wall samples include
waits and do not establish a CPU percentage or any speedup from this kernel.

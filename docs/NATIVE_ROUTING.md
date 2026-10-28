# Exact native routing

P6 evaluates burndown VB06 and the remaining production bookkeeping in VB07.
It preserves the generated function bodies' guest state and observation gates.

The complete, unmodified exact-PC route table remains authoritative. A static
open-addressed index maps full 32-bit PCs to its entries, at no more than 50%
occupancy. Equality compares the complete address; odd PCs, aliases that were
not generated, and uncompiled addresses still miss. Empty index entries use
zero, so guest PC zero is representable. There is no runtime allocation or
mutable dispatch cache. `VBRECOMP_NATIVE_DISPATCH=OFF` retains binary search.

A generated JAL can supply a native function pointer only when its complete
target address is a compiled function entry. The fast call preserves the link
write, target-equals-return no-op, halted/yielded gates, interpreter override,
native-entry counters, subsequent transfers and ordinary dispatch-loop unwind.
Interior and unknown targets use the normal exact-PC path. Function-entry
knowledge spans all generated translation units, including split builds.

Production builds already replace tracing with empty implementations. Inline
event wrappers remove these empty calls and their argument preparation when
debug tooling is disabled. Tooling builds retain the actual rings and ABI.
Custom memory buses, MMIO and write observers are unchanged by this package.

Portable validation generates 2,053 sparse full-address routes, including zero
and address aliases. Both indexed and binary-search builds check one million
random lookups and every known/adjacent/alias address. Each configuration also
compares 40,000 known calls and 40,000 unknown calls, across execution modes,
yield/stop gates, budgets, deadlines and repeated transfers. The normal engine
suite passes 24 tests; the Python suite passes 84 tests with five unavailable
oracle fixtures skipped. Real title generation/build and matched game gates
remain necessary; these fixtures do not establish a game performance win.

Campaign evidence is retained privately under the owning issue `beads-vb.1.16`;
this manifest contains no game binary or cartridge content.

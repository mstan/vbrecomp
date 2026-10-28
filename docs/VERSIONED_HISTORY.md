# Qualified frozen pages in private history

P9 connects VIP capture ownership to the private immutable history builder
(VB10/VB11). Post-P8 Wario profiling still placed1030/6270 inclusive wall samples
under rewind and448 CRT-caller samples under page interning. These are selection
evidence, not CPU-time percentages or a measured P9 speedup.

A capture-only visitor supplies stable bank-page addresses and qualified nonzero
revisions. The builder reuses an owned immutable page when both identity and
revision match a live weak reference. It does not hash or compare those bytes
again. Changed/unqualified/expired inputs use the original full-equality content
index. The version index is bounded and owns no page data. Address hashing mixes
alignment bits and has no host instruction-set requirement.

Only canonical VIP DRAM/CHR captures receive this qualification. Bank rotation
keeps the identity attached to the physical bank. External-input captures and
restored banks remain unqualified until recaptured. VIP reset/load advances a
process-local monotonic revision clock. At exhaustion, complete-copy/equality
fallback remains functional. Revisions and cache identities stay outside guest,
save and network data.

The ordinary visitor and public contiguous state encoding remain unchanged.
Private capture visits qualified4KiB spans and the remaining metadata in the same
byte order. `VBRECOMP_VERSIONED_HISTORY=OFF` retains the prior immutable-page
implementation. Existing expanded provenance and arbitrary renderer callback
semantics remain intact.

Tests compare every qualified visitor byte with the ordinary viewport byte
stream, including bank rotation, arbitrary-source fallback, load/reset and clock
exhaustion. Page ownership, weak expiry, bounded lookup, colliding contents,
partial prefixes/suffixes and1000 random changed/unchanged captures are checked
against independent saved byte vectors. Current-build state-extension checks and
the existing scalar/tile viewport differential also pass. Built-game qualification
and matched per-game timing remain pending; no P9 win is accepted yet.

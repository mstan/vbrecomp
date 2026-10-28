# Versioned frozen capture pages

P7 evaluates burndown VB10 without changing a title's rendering algorithm or
the contiguous read-only DRAM/CHR arrays supplied to existing callbacks.

Only canonical VIP-owned source pointers qualify. Forty host-derived revisions
cover 4 KiB pages of the 128 KiB DRAM and 32 KiB CHR inputs. Every aligned bus
write to either range, including CHR aliases and byte/halfword/word accesses,
changes the affected page revision. Device rendering writes framebuffer ranges,
which are separate from these inputs. State loading and VIP reset invalidate all
bank keys. Revision wrap also invalidates keys before reusing a clock value.

Each physical frozen bank retains its own revision keys, including the spare
bank used to preserve borrowed rows. A capture copies changed pages and keeps
unchanged bytes in that same contiguous bank. Metadata is still copied at every
original capture point, and its callback still runs. The preceding borrowed bank
remains alive through replacement. External source pointers always take the
complete-copy fallback and invalidate that bank's keys.

Revisions, clocks and validity keys are host caches, not guest or save-state
content. Loading a current-build snapshot rebuilds their validity independently
of restored frozen bytes. `VBRECOMP_VERSIONED_CAPTURE=OFF` retains complete
copies. Games that never request viewport tracking do not maintain revisions.
Diagnostic totals count actual copied bytes and reused pages.

Focused tests compare every frozen byte against live canonical inputs, exercise
all forty pages and four CHR aliases, bank rotation/retained ownership, external
input mutation, reset/load invalidation and clock wrap. Existing eager/deferred
rasterizer checks also pass. Full built-game qualification and per-game matched
performance remain pending; copied-byte counts alone are not a runtime win.

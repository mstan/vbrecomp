# Versioned frozen capture pages

P7 evaluates burndown VB10 without changing a title's rendering algorithm or
the contiguous read-only DRAM/CHR arrays supplied to existing callbacks.

Only canonical VIP-owned source pointers qualify. Forty host-derived revisions
cover 4 KiB pages of the 128 KiB DRAM and 32 KiB CHR inputs. Every aligned bus
write to either range, including CHR aliases and byte/halfword/word accesses,
changes the affected page revision. Device rendering writes framebuffer ranges,
which are separate from these inputs. State loading and VIP reset invalidate all
bank keys and advance the clock. Tokens are never reused; counter exhaustion
selects complete-copy fallback for the remainder of the process.

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

Frozen banks also retain exact CHR tile hashes across native drawing, both eyes
and repeated viewport presentations (VB13). Copying a changed CHR page clears
only its tile-hash validity; reset/load and arbitrary-input captures invalidate
the corresponding complete keys. Hashes are never guessed from a partial key.
Unowned artwork retains the original hash path. The borrowed old bank retains
its hashes through rotation. These caches remain outside serialized content.

Thumbnail capture reuses one working vector (VB21), retaining the original
zero initialization on every capture. Shutdown frees its capacity. It does not
reuse rendered images or assume that arbitrary renderer callbacks are pure.

Focused tests compare every frozen byte against live canonical inputs, exercise
all forty pages and four CHR aliases, bank rotation/retained ownership, external
input mutation, reset/load invalidation and clock wrap. Existing eager/deferred
rasterizer checks pass; every tile hash is checked against the original byte
hash after mutations, across retained banks and repeated reads. Full built-game qualification and per-game matched
performance remain pending; copied-byte counts alone are not a runtime win.

# Native performance campaign: Wario Land and Zero Racers

Tracking: `beads-4fpw`, related to `beads-2wy8.1` (`optimization campaign`).
Source and raw evidence: `G:/Temp/Codex/virtualboyrecomp/native-services-20261026`.
Branches: engine `perf/native-services-20261026`, Wario `perf/native-rendering-20261026`.

The integrated reference retains current shared-engine lifecycle behavior and
current Wario entity/viewport behavior: shared `9919ead`, Wario `7bb9442`, private
framework `e313b91`, UI `c5f05e7`, integrated baseline `548d120`. Measurement-only
support is applied equally to both arms. Existing checkouts and owner saves are
preserved. Work is solo; timed workloads own the shared per-host benchmark lock.

## Acceptance and evaluation

Evaluate every row below against actual built-game costs. Evaluation can finish
with a demonstrated improvement, neutral result, rejection or evidence that the
service is cold; it does not require implementing every proposed cache. Reassess
the approach after two unsuccessful experiments. Preserve useful gains even
when a companion title is neutral. A reproducible regression in an individual
game must be fixed or removed; aggregate gains do not compensate for it.
Identical baseline/candidate defects are tracked separately.

Primary workload is Wario's 3,450-frame torch/bat/interior route with color,
adaptive widescreen at an actual 21:9 per-eye window, both eyes and audio
synthesis. Stock and independently selected mods are correctness companions.
Zero Racers follows with its recorded driving/pause route. Package names in
the final machine-readable results distinguish title-specific color work from
shared engine work. No improvement is claimed before uninstrumented A/B.

## Opportunity burndown

| ID | Opportunity and removed work | Current disposition / next evidence |
| --- | --- | --- |
| R1 | Resolve material masks by content once; precompute color/intensity output | Retained in both titles, with distinct transfer rules: Wario uses /255; Zero normalizes by max RGB. Wario has 196,608 scalar comparisons; Zero has 1,073,152. Combined Wario V4 saves 25.984% CPU; Zero shared V4 plus title Z1 saves 23.610%. Package gains are not attributed individually. Actual routes, both eyes, PCM and lifecycle checks pass. |
| R2 | Render captured BG/HBias inputs as tile-row jobs instead of per-pixel map/CHR lookup | Implemented captured tile-row jobs; 384 differential scene/eye/width checks and built-game output agree. Retained in V2/V4. Zero has no expanded viewport policy on the selected route; validate its shared package separately. |
| R3 | Prepare OAM row lists once per captured eight-row block | Evaluated, not selected: Wario native OBJ is 0.416% plus viewport OBJ 0.274% of source-resolved samples. Zero's entire inlined VIP tick region is 6.132%; this is an upper bound, not an OBJ attribution. Existing native row decoding is retained; no additional OAM index or ordering/invalidation contract is introduced without a demonstrated gain. |
| R4 | Reuse affine cell/row decoding across adjacent samples | Evaluated, not selected: Wario native affine drawing is 0.115% of samples. Zero's unresolved inlined VIP tick region cannot be credited to affine drawing. Retain existing row decoding and exact wrap, overplane, signed/per-eye behavior; no speculative affine cache. |
| R5 | Share decoded drawing jobs between native framebuffer and expanded replay | Architectural evaluation: native framebuffer bytes and CPU-write provenance remain observable guest memory. Replacing native drawing with replay is rejected. Existing native eight-pixel row decoding is retained; captured-input batching is implemented as R2. A new shared drawing IR is not selected for the measured native BG cost (5.02%) and differing frozen/live ownership. |
| R6 | Reduce repeated full DRAM/CHR/extra snapshot copies | Copy-elision evaluation: captures must freeze mid-frame DRAM/CHR/extra inputs and survive fresh-process load. Simple copy removal is rejected. The capture function itself is 0.062% of Wario samples; CRT copy time is not assigned to a caller without stacks. Persistent-page sharing would require a different snapshot ownership backend; no unqualified memory/lifetime rewrite adopted. |
| R7 | Reduce provenance hashing, buffer clearing and output copies | Provenance evaluation: native source production 3.50%, block packing 3.54% in Wario source-resolved samples. Native CHR hashes already share an eight-row block across eyes. Required planes and CPU-write ownership are preserved. Extra cross-capture caches require invalidation and increase retained history; prior row-copy experiment X3 had mixed results. No second unqualified plane-layout or copy rewrite adopted. |
| R8 | Hoist stock brightness/gamma/red-LUT work to column palettes | Implemented all LED-model mapping once per 96x4 column palette. Varying brightness, both eyes/slots and warm transfer checks pass. Retained in measured V2/V4 packages; no individual percentage claimed. |
| R9 | Resolve Wario streamed margin layout and immutable ROM cells once per captured block | Implemented Wario render-call views of captured layout and immutable cells; 800,904 scalar comparisons pass. Baseline outside-cell service was 9.17% of history-off samples. Retained in measured V2/V4; pointers remain derived after restore. |
| C1 | Remove repeated per-instruction bookkeeping from native regions | Retained production-only debugger guard elision; device budgets and simulation hooks remain. Debug breakpoint precedence and stripped-production simulation redirects pass. Actual Wario parity and Zero native/interpreter 4-player session pass with production instruction guards selected in observation builds. Both measured packages are positive. |
| C2 | Replace repeated binary exact-PC dispatch lookup | Evaluated, not selected: exact-PC lookup is 1.09% of Wario samples and 3.479% of Zero samples. The generated Zero table includes 46,739 arbitrary resume PCs, not only function starts. Retain exact lookup and alias/fallback behavior; an extra mutable dispatch cache has a small total upper bound compared with the delivered 23.6-26% package gains. |
| C3 | Native ROM/WRAM/SRAM fast access | Retained portable ordinary-memory service, qualified ON in both measured packages and enabled by default. 1,196,544 original/direct comparisons cover aliases, alignment, live data, oversized/non-power-of-two ROM and MMIO deadlines. Writes/observers/cycle charges remain. Both titles pass state/PCM/pixel parity and have positive matched CPU results. The legacy implementation remains selectable. |
| C4 | Batch scheduler/device work until observable deadlines | Evaluated low cost: Wario scheduler/devices 0.73%; source-resolved VIP tick 0.097% and column advance 0.027%. Zero scheduler/devices is 0.812%. Retain existing observable-deadline batching. Inlined raster work inside VIP tick is not scheduler cost. |
| C5 | Filter negative branch-policy lookups before calling the runtime table | Prototype bitmap filter passes 131,072 original-rule comparisons but is parked: Wario lookup cost is 0.42% of normal-history samples, below current whole-game variation. Zero does not compile the Wario branch-policy override service. No speculative filter adopted. |
| C6 | Stop hashing WRAM after first-N diagnostics freeze | Retained in V4: preserve all 32,768 diagnostic records and reset behavior, avoid preparing hashes once the ring freezes. Directed full/frozen/reset tests pass. Cold in short game routes; no whole-game percentage is credited to this guard. |
| A1 | VSU event-span/block synthesis | Evaluated cold: existing synthesis already advances event spans. Audio is 0.17% of Wario and 0.406% of Zero normal-history samples. No new replacement. Absolute PCM windows match across actual routes/restores; Zero four-player native/interpreter sessions pass. |
| H1 | History capture/serialization at real player cadence | Prior payload/history optimization remains and is not re-credited. Current profile separates actual first-N/capture work from the avoidable UI scan H3. Normal 32-entry/six-frame capture is measured. Shared snapshot-cap startup failure is fixed in both arms (beads-6rw9). No captured presentation is dropped to obtain speed. |
| H2 | Host texture upload, presentation, allocation and driver overhead | Evaluated host boundary: CPU runs retain software composition/upload/present and audio synthesis, but exclude physical GPU/speakers. DLL RVAs and module hashes are retained; nearest exports point mainly to CRT moves/clears, virtual-memory frees and waits. Export distances are not caller stacks. Keep tuned portable libc and current snapshot ownership; no GPU/driver speed claim or speculative assembly/backend replacement. |
| H3 | Refresh history UI membership only after a mutation | Retained: redundant reconciliation consumed about 6.4% of Wario samples and 11.3% of Zero samples. A host-only rewind revision invalidates previews on store/eviction/drop/load/reset/shutdown. Network completion still polls continuously. Actual slots/filmstrip, fresh restore and two-peer room/state/rematch tests pass. Both measured packages are positive. |
| X1 | Bit-string instruction replacement | Rejected atomic bit-string HLE: it changes interrupt/preemption boundaries. Historical logical operations were cold on Zero/Mario routes; current Wario profile does not justify replacing the interruptible service. Preserve the functioning original path. |
| X2 | Interpreter fetch/decode batching | Evaluated cold on both recorded game routes: Wario and Zero run with zero interpreted/fallback instructions. The explicit interpreter remains for qualification and portable fallback; batching it cannot improve these native workloads. |
| X3 | Prior VIP attribution-row batching | Historical row-batching result was mixed (-2.36% FPS / 0.59% CPU saving) with unmatched compilers. No proven win is claimed and the old implementation is not imported. Current source attribution is recorded under R7. |

## Qualification and handoff

Keep a functioning reference binary. Run focused component comparisons plus
matched game routes, both-eye output and current-build restore/rewind/native
save checks. Validate independently selectable stock/color/wide modes and
adaptive geometry. Reuse prior evidence; expand only for concrete risks.

Timed runs retain raw samples, identities, feature settings, useful frame/audio
counts and host census. A/A and reversed A/B pairs distinguish gains from
variation. The retained resource ledger identifies disposable generated/build
data. The final owner handoff uses normal windows, standard controls and normal
audio; no uncapped or dummy-output configuration is handed to the owner.

Owner acceptance that the games look/play good authorizes merge of qualified
engine/title changes to main/master and new verified title builds pinned to the
landed engine. Deliver one current archive per game and preserve user saves.
Keep machine-readable data and reproducible visual inputs for the campaign;
do not publish a blog post without a separate request.

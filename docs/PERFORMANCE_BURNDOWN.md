# Accuracy-preserving performance investigation

Owner request: 2026-10-22. Central issue: `beads-cai4`, under
`beads-vb.1` / `beads-vb`. Work is solo. Initial engine: `1dac760`.
Branch: `codex/accuracy-perf-20261022`. Final integration requires owner approval.

The final review is based on public engine `8e8d3c6`, game `817e140`, and
its UI pin `c5f05e7`. Unpublished Wario work remains in its original workspace.
Only the bounded per-eye column-brightness correctness dependency from the
initial workspace is carried here. No game-specific Wario changes are imported.

## Implementation and coverage inventory

| Area | Current implementation | Evidence and limits |
| --- | --- | --- |
| V810 | Generated C with exact-PC dispatch; interpreter handles missing ROM entries and RAM code. Shared extended operations and guest-cycle model. | Recompiler tests, interpreter/native bridge test, `execution_stats`, independent Beetle traces. Hybrid/interpreter agreement alone is not independent accuracy. |
| Scheduler | Guest deadlines, MMIO synchronization, HALT event stepping, IRQ delivery. | Canonical CPU/device fields and VIP phase ring available over TCP. Preserve deadlines and guest timing. |
| VIP drawing | 28 eight-row blocks, worlds descending 31..0, BG/H-bias/affine/object paths; two drawing/display framebuffer slots, each with both eyes. | Existing synthetic source/viewport raster tests; independent full-memory and both-eye comparisons. No cross-block world/CHR stability assumption is valid without checking writes. |
| Decoding | BG aligned eight-pixel path already exists. Affine map lookup is per pixel. Objects reuse a packed character row across eyes; object descriptors are scanned per row. Palette values are already cached. | Character, map, OAM and world data remain writable via canonical and aliased bus addresses. Any persistent decode cache needs exact invalidation. |
| Presentation | Stock 2bpp-to-ARGB expansion, per-eye CT brightness groups, gamma and optional red LED lookup. Recolor/custom renderer and adaptive replay are separate optional paths. | Stock expansion calls `vb_red_lut_map` once per pixel. Proposed lookup hoist was removed from the final candidate. |
| Copies/allocations | Drawing uses static row/source buffers. Adaptive replay copies 128 KiB DRAM + 32 KiB CHR + palettes/OAM suppression per block when tracking is enabled. Custom presentation copies stock output. | Measure with actual enabled features; turning these off is not an optimization. No allocation in stock VIP drawing/expansion loops. Broader allocation attribution remains to measure. |
| Audio | Six-channel VSU, band-limited stereo output, emulated time and host delivery. | PCM capture and canonical device fields in existing cosim harness. Mute unattended output while retaining synthesis. |
| Persistence/lifecycle | Initially native SRAM only; mod-runtime `save_state` stored settings. This branch adds current-build execution snapshots, local rbengine history, reset and deterministic frame replay. | See `LOCAL_STATE.md`. No recomp-net, network rollback session, shared-controller play or link emulation. Netplay was explicitly deferred. Old snapshot compatibility is irrelevant. |
| Reference | Separate Beetle VB process, pinned observation patch, byte-audited state/images/PCM comparison. | Prior `docs/PARITY.md` records Zero Racers 39 checkpoints / 4,000 frames and Mario's Tennis regression; these are historical, not results for the new branch. |

Proving ground: existing Zero Racers `tests/race-driving-route.json`, including
menus, driving, steering and pause. Existing ROM/generated corpus is at
`F:/Projects/virtualboyrecomp/_wt-zero-racers` and remains preserved. After
rebasing onto the current public engine, both corpora were regenerated with
that unchanged generator into G: scratch. Mario's Tennis is the regression title.

## Task burndown

| Task | Status | Exit evidence |
| --- | --- | --- |
| Confirm repository, instructions, unrelated changes, storage and issue | Done | Engine master confirmed; unrelated untracked coverage probe preserved. Isolated worktree and all scratch/TEMP on G:. |
| Inventory implementation and real coverage | Done | Table above; historical evidence distinguished from new qualification. |
| Establish fixed baseline and reproducible feature configuration | Done | Frozen Release/GCC 15.2 arms, debug/CPU-hook off, same generated Zero Racers corpus and enabled features. Binary hashes below. |
| Profile CPU/dispatch, scheduler, VIP modes, both eyes, copies, allocations, audio and host presentation | Done with attribution limits | Perturbed main-thread PC samples plus source audit; DLL/internal allocator attribution and physical GPU/audio delivery remain unverified. |
| Add local rbengine lifecycle | Automated qualification complete | `beads-8m99`; standalone snapshot-ring module only. `beads-7plg` tracks companion game presentation spans. |
| Test bounded candidates | Done | Ledger below; no speculative decode/display-list rewrite. |
| Balanced A/A and A/B CPU measurements | Passed before menu integration | Depth32 CPU reduction 37.15%, A/A 0.35%, both pair directions agree; same source/features except serializer. New UI overhead remains unmeasured. |
| Automated qualification | Passed | Independent Zero Racers and Mario routes, both eyes, full state/PCM; stock/color/portable lifecycle, actual build-configuration rejection, native persistence, CTest and recompiler tests. |
| Combined normal-window, normal-audio, one-eye playtest | Ready for human review | Qualified executable and isolated profile staged. No final integration or original-worktree changes. |

## Candidate ledger

| Candidate | Outcome | Evidence |
| --- | --- | --- |
| Prior VIP row/source batching (`abc12ab`, separate existing worktree) | Rejected historically | Prior whole-runtime pair: -2.36% FPS, 0.59% process CPU reduction, foreign compilers present. Do not promote or silently fold into this investigation. |
| Draw-local affine map lookup reuse, persistent CHR/BG/object decode or display-list caches | Unverified / not selected | Reviewed existing aligned-row/object reuse and live writable maps. Profile did not justify added retained state/invalidation machinery. No general-game win claimed. |
| Immutable adaptive replay snapshots | Unverified / not selected | Large repeated snapshots exist only with tracking; no feature toggles or assumed SNES transfer. Existing implementation retained. |
| Stock presentation palette resolution | Removed from final candidate | Initial A/A 4.59% exceeded the predeclared 3% gate. Longer follow-up was interrupted by a foreign benchmark; no accepted standalone speed claim. |
| Single-allocation snapshot payload + private history CRC elimination | Accepted for owner review | Final depth32 CPU reduction 37.15%, A/A 0.35%, pair reductions 37.21%/37.09%. Exact counted payload replaces vector growth and a second full copy. Private immutable entries omit redundant CRC; external input retains it. Read-only capture excludes the host audio cursor and needs no SDL audio lock. Individual contributions were not separated. |
| Smaller caches and palette/OAM deduplication | Not selected | SNES rejection is methodological context only; no Virtual Boy benchmark outcome claimed. |

## Measurement rules

Use identical enabled features, compiler optimization, route, guest work and
audio synthesis in each arm. Report process CPU and wall throughput separately.
Run A/A noise control and balanced A/B then B/A; predeclare run count rather
than repeating until favorable. Serialize game benchmarks and check competing
compiler/game jobs and F:/G: load before runs. Preserve healthy foreign jobs;
do useful preparation while they finish. Unattended launches are muted.
Capture files and logs stay on G:, outside version control. Both eyes are
checked independently, not inferred from the displayed left eye. Any omitted
host/GPU/audio-device work must be stated, never called full-runtime evidence.

## Results and reproducibility

Evidence root: `G:/Temp/Codex/virtualboyrecomp/accuracy-perf-20261022`.
The original F: engine's untracked `tools/coverage_probe.py` is preserved.
`F:/NAS` was inspected as a normal directory on F:, not assumed to be another
physical disk. G: had ample free space; TEMP/TMP and Python caches stayed there.

Production profiles (`profile-prod-stock`, `profile-prod-rewind`) are perturbed
samples, not speed results. Stock: 2,553 samples / 4 errors, red mapping 303,
VIP expansion 92, VIP ticking 64, WRAM observation hashing 49, VSU update 15.
Rewind: 3,996 / 4 errors, CRC 1,222, snapshot field insertion 107, mapping 279,
VIP ticking 71, expansion 68, dispatch 17. The largest groups are unresolved
system-library samples (stock ntdll 1,659; rewind 1,762). They do not establish
an allocator or synchronization cause. Stock drawing/presentation use static
scratch; the old snapshot payload grew a vector, allocated a second blob and
copied it. Small feature-manifest allocations remain. No broad heap-allocation
reduction, world-decoding speedup, or V810 dispatch improvement is claimed.

`measure-fixed-headroom/results.json` records the older pre-rebase combined
palette-plus-serializer candidate, not the final review head. Its complete series:
warm A/B, A1/A2 control, then A3/B1/B2/A4 for each depth. Windows 9800X3D,
GCC 15.2 Release, fixed affinity `0x3c00` (two physical cores), stereo stock
renderer, mods off, audio synthesis on. Software SDL dummy video/audio avoid
speaker output and interference with other playtests. GPU and physical audio
delivery are excluded. Each run starts with measured CPU <=60%, free RAM
>=4 GiB and queue G<=2/F<=8. Starts were deferred when needed; other jobs were
preserved. One post-run load spike at depth32 is recorded, not hidden. A global
cooperative benchmark lock and active-process census serialize detected work.

| History | Baseline CPU s | Candidate CPU s | Reduction | A/A variation | Pair reductions |
| --- | ---: | ---: | ---: | ---: | --- |
| Off | 5.58594 | 4.29688 | 23.08% observed | 4.59% | 25.47%, 20.52% |
| 32 frames | 10.49219 | 4.89844 | 53.31% | 0.91% | 52.76%, 53.87% |

All 16 runs perform the same 4,000 frames, 1,591,296,002 guest cycles,
888,495,917 native and 23,922 interpreted/fallback instructions, 3,999 stereo
presentations and 3,508,807 synthesized audio frames. The terminal guest frame
exits before its presentation. WRAM SHA256 is
`505a56de87d4e63b89291480a1694934423540563234ffa6075abef680f5082d`.
The history benefit compares two implementations of the same enabled feature;
it is not a speedup against the original engine with no history.

Frozen baseline SHA256:
`9d01e3aca883d2215e56c45861cde0b1513a4a0e3926d279dcd4fae697e9ea60`.
Frozen candidate:
`fc1079abac17c39f4a596297228bc07e0cee6d9ffb2853141b3da934483b0843`.
Both arms include the same local lifecycle functionality. Subsequent file-error
handling, explicit 512-slot bounds, custom-source build-identity registration
and discovery documentation do not change the timed depth0/depth32 path.
Do not reuse that 53.31% result for the final head: palette hoisting was removed
and current upstream corpus coverage changed. Pre-menu debug/UI game host SHA256:
`116bb55548ba3dc5a621e6ee1dea2d0203e01c413fb0bd90d3c1d4a64602cc01`.
Its UI, custom color renderer and normal audio driver are covered by separate
correctness/playtest checks, not these production CPU numbers.

`measure-balanced-checked` stopped on foreign build overlap;
`measure-headroom-pinned` stopped on an aggregate CPU spike;
`measure-fixed-final` refused its initial headroom check. Other diagnostic
attempts are retained. Warmups/partial series are not accepted performance
evidence. The completed series measures aggregate CPU using GetSystemTimes
over the census interval, retains the instantaneous WMI counters too, waits
for eligible starts and records post-run spikes. No run is repeated until
favorable, no workload difference is accepted, and profiling is never mixed
with throughput measurement.

`measure-palette-final` tried longer fixed samples, but a foreign
`SuperMarioWorldSNESRecomp --benchmark-audio 4000` overlapped its A/A control.
The series stopped and was preserved; no favorable-run retry or accepted result.
The palette hoist was consequently omitted from the review candidate.

The final `review-measure-history` comparison uses current fully AOT Zero
Racers code (888,519,839 native instructions, zero interpreted/fallback),
32-frame history in both arms, and one fresh 4,000-frame route per sample.
Only the earlier serializer differs in the baseline. The upstream coverage
improvement is not attributed to this optimization. Acceptance requires A/A
<=3% and both pair reductions greater than twice the measured noise.

Final pre-menu result: baseline mean CPU 13.1640625s, candidate 8.2734375s, reduction
37.1513%; A/A variation 0.3474%; paired reductions 37.2121% and 37.0930%.
All eight runs completed, including the predetermined warmups and control.
One post-run CPU headroom spike is retained; no foreign benchmark overlapped.
Affinity was `0x3c00`. Each run performed 1,591,296,002 cycles, 3,999 stereo
presentations and 3,508,807 synthesized audio frames. Final WRAM SHA256 was
`505a56de87d4e63b89291480a1694934423540563234ffa6075abef680f5082d`.
Final baseline executable SHA256:
`51792c30bc57fcb3a1db9807ee10c7b309cc62f68f41cb2a30d969fc7f05796f`;
candidate: `d31e53943287e1d33df2b2c2c295144cc92f069b7ac0385ce70c3dfde31d50f2`.
This is the cost reduction of two implementations of enabled local history,
not a general speedup over the original engine with rewind disabled.

## Automated qualification evidence

| Evidence directory/log | Result and scope |
| --- | --- |
| `review-reference-driving` | PASS 39 checkpoints / 4,000 frames / 273 byte audits; independent Beetle CPU, full memory/device state, both eye images, presented output and stereo PCM. |
| `review-reference-mario` | PASS 15 checkpoints / 1,000 frames / 105 byte audits; same ten planes and current generated code. |
| `review-lifecycle-stock`, `review-lifecycle-color` | PASS 36 checks each; current game-host lifecycle, both eyes and source/provenance bytes, active color mod separately; mid-frame dispatch, coalesced debugger PC edit/replay, replay PCM, all four framebuffer buffers, aliased CHR, reset, repeated presentation, rewind, feature/build/content/CRC rejection, atomic save failure. |
| `review-native-save` | PASS seven checks: exact load/save, old execution SRAM restore, newer native progress backed up, invalid native input rejected unchanged. Only isolated agent-owned files. |
| `review-ctest.log` | PASS 7/7, including native/interpreter bridge, interpreter stock, VIP sources/viewport/brightness and mod runtime. |
| `review-recompiler-tests.log` | PASS 82; 5 optional oracle tests skipped explicitly (87 total). |
| `review-cli.log`, `final-route-adapter.log` | Malformed finite workloads rejected, including conflicting frame limits; route conversion checks pass. |
| `review-lifecycle-msvc`, `review-build-identity` | PASS 36 lifecycle checks in MSVC x64 SDL-free interpreter host; PASS 6 actual Release/Debug layout/content/feature identity, distinct build IDs, atomic mutual rejection and own-file acceptance checks. No non-Windows or cross-build compatibility claim. |

Native SRAM and old disposable execution snapshots have separate contracts.
All automated launches were headless or used dummy/zero-volume output. The
owner handoff uses a normal window, real audio driver and volume100 with the
combined local-history changes. Human controls/audio/feel and final integration
remain pending; network rollback, netplay and link cable behavior are absent.

## Owner-requested F7/F8 host integration

The owner approved gameplay, then identified that the first handoff had raw
backend controls instead of the established PSX/SNES menus. That handoff did
not complete the player-facing lifecycle integration. Inspection confirmed
PSX's `runtime/src/psx_savestate_menu.c` and SNES's
`runner/src/snes_savestate_menu.c` / `snes_rewind.c` own their state machines
and raster overlays; recomp-ui itself does not supply these browsers.

Virtual Boy now follows that host pattern in one framework module, so future
game hosts inherit it: twelve thumbnail/timestamp slots on F7, F8 rewind
filmstrip with explicit select/cancel, controller navigation and Select+R/L
gestures, modal execution/audio policy and a release guard for menu inputs.
Browsing never restores machine state. New Settings actions and history
enable/depth/interval controls accompany it. Interactive history defaults to
32 snapshots every six frames; headless/finite workloads retain opt-in history.
F5/F6 are quick save/load. No game renderer was replaced.

`ui-final-menu-stock` and `ui-final-menu-color` each pass 21 checks through actual SDL keyboard/controller-button
events and real snapshot files/ring entries. It covers Unicode profiles,
twelve-slot navigation, bound thumbnails, running-guest modal freeze, complete
state preservation while browsing/canceling, save/load failures, rewind commit
and future truncation, reset invalidation and damaged input. Dummy output and
a process-local SDL controller filter prevent physical input from interfering with
automation. OS-held keys and physical controller drivers remain human QA.
The initial Unicode startup failure is retained in `ui-menu-qualified`;
Windows argv and host filesystem boundaries now use UTF-8 consistently.

The combined menu build also repeats stock/color lifecycle and independent
reference qualification under `ui-final-*`: stock/color/MSVC each pass36
lifecycle checks; Zero Racers39 checkpoints/273 byte audits and Mario15/105
match the independent reference; native persistence7 and MSVC Unicode-native
persistence7 pass; actual Release/Debug identity6, CTest7/7 and finiteCLI pass.
Production MinGW and MSVC Release/Debug build successfully. The preceding 37.15% measurement
qualifies the serializer implementation before thumbnail/menu integration;
it does not measure the new UI's incremental capture cost or its six-frame
interactive cadence. That overhead remains unmeasured. No new overall CPU
percentage is claimed for this UI follow-up.

Full regression menu build SHA256:
`c1c1c779e1b2e37291afac99e3ec0dbeff669563f754d77032df76c6adeae99d`.
Delivery build after glyph-header line-ending normalization:
`72cfe70cd85f70293d3bdee210350a35b44ead175aa67ae749f0b7685aa54d81`.
`font-normalization-proof.json` confirms identical executable code sections;
only `.rdata` changed with the source-derived snapshot build identity. The
stock/color 21-check current-build menu suites were repeated against the
delivery binary in `ui-delivery-menu-stock` and `ui-delivery-menu-color`.

Handoff: `final-review/Playtest.ps1` inside the evidence root. One eye, color mod on,
32-snapshot history at six-frame intervals, isolated SRAM/config/snapshot paths. The real Windows
audio driver is restored; software SDL presentation avoids driver shader-cache
output on C:. TEMP/TMP stay under the handoff directory. The executable is the
same qualified menu build named above. Gameplay has owner approval; final UI
checking should cover F7 save/load, F8 scrub/cancel/select, controller gestures,
input release and audio resumption. Both eyes were checked independently by
automation; the owner display deliberately shows one eye. No native owner save
is used. Non-Windows builds remain unverified; no mandatory host-specific
backend, assembly or SIMD was introduced.

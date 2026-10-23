# Accuracy-preserving performance investigation

Owner request: 2026-10-22. Central issue: `beads-cai4`, under
`beads-vb.1` / `beads-vb`. Work is solo. Baseline engine: `1dac760`.
Branch: `codex/accuracy-perf-20261022`. Final integration requires owner approval.

## Implementation and coverage inventory

| Area | Current implementation | Evidence and limits |
| --- | --- | --- |
| V810 | Generated C with exact-PC dispatch; interpreter handles missing ROM entries and RAM code. Shared extended operations and guest-cycle model. | Recompiler tests, interpreter/native bridge test, `execution_stats`, independent Beetle traces. Hybrid/interpreter agreement alone is not independent accuracy. |
| Scheduler | Guest deadlines, MMIO synchronization, HALT event stepping, IRQ delivery. | Canonical CPU/device fields and VIP phase ring available over TCP. Preserve deadlines and guest timing. |
| VIP drawing | 28 eight-row blocks, worlds descending 31..0, BG/H-bias/affine/object paths; two drawing/display framebuffer slots, each with both eyes. | Existing synthetic source/viewport raster tests; independent full-memory and both-eye comparisons. No cross-block world/CHR stability assumption is valid without checking writes. |
| Decoding | BG aligned eight-pixel path already exists. Affine map lookup is per pixel. Objects reuse a packed character row across eyes; object descriptors are scanned per row. Palette values are already cached. | Character, map, OAM and world data remain writable via canonical and aliased bus addresses. Any persistent decode cache needs exact invalidation. |
| Presentation | Stock 2bpp-to-ARGB expansion, per-eye CT brightness groups, gamma and optional red LED lookup. Recolor/custom renderer and adaptive replay are separate optional paths. | Stock expansion calls `vb_red_lut_map` once per pixel. Candidate: resolve its four possible outputs per group per call, with no retained state. |
| Copies/allocations | Drawing uses static row/source buffers. Adaptive replay copies 128 KiB DRAM + 32 KiB CHR + palettes/OAM suppression per block when tracking is enabled. Custom presentation copies stock output. | Measure with actual enabled features; turning these off is not an optimization. No allocation in stock VIP drawing/expansion loops. Broader allocation attribution remains to measure. |
| Audio | Six-channel VSU, band-limited stereo output, emulated time and host delivery. | PCM capture and canonical device fields in existing cosim harness. Mute unattended output while retaining synthesis. |
| Persistence/lifecycle | Initially native SRAM only; mod-runtime `save_state` stored settings. This branch adds current-build execution snapshots, local rbengine history, reset and deterministic frame replay. | See `LOCAL_STATE.md`. No recomp-net, network rollback session, shared-controller play or link emulation. Netplay was explicitly deferred. Old snapshot compatibility is irrelevant. |
| Reference | Separate Beetle VB process, pinned observation patch, byte-audited state/images/PCM comparison. | Prior `docs/PARITY.md` records Zero Racers 39 checkpoints / 4,000 frames and Mario's Tennis regression; these are historical, not results for the new branch. |

Proving ground: existing Zero Racers `tests/race-driving-route.json`, including
menus, driving, steering and pause. Existing ROM/generated corpus is at
`F:/Projects/virtualboyrecomp/_wt-zero-racers`; do not regenerate or alter it
unless a generator change requires it. Mario's Tennis is the regression title.

## Task burndown

| Task | Status | Exit evidence |
| --- | --- | --- |
| Confirm repository, instructions, unrelated changes, storage and issue | Done | Engine master confirmed; unrelated untracked coverage probe preserved. Isolated worktree and all scratch/TEMP on G:. |
| Inventory implementation and real coverage | Done | Table above; historical evidence distinguished from new qualification. |
| Establish fixed baseline and reproducible feature configuration | Done | Frozen Release/GCC 15.2 arms, debug/CPU-hook off, same generated Zero Racers corpus and enabled features. Binary hashes below. |
| Profile CPU/dispatch, scheduler, VIP modes, both eyes, copies, allocations, audio and host presentation | Done with attribution limits | Perturbed main-thread PC samples plus source audit; DLL/internal allocator attribution and physical GPU/audio delivery remain unverified. |
| Add local rbengine lifecycle | Automated qualification complete | `beads-8m99`; standalone snapshot-ring module only. `beads-7plg` tracks companion game presentation spans. |
| Test bounded candidates | Done | Ledger below; no speculative decode/display-list rewrite. |
| Balanced A/A and A/B CPU measurements | Complete | Fixed two-physical-core affinity, full-route warmups, A/A then A/B/B/A, depth 0 and 32, identical work. Interrupted attempts retained separately. |
| Automated qualification | Passed | Independent Zero Racers and Mario routes, both eyes, full state/PCM; stock/color lifecycle, mid-frame restore, direct writes, reset, atomic rejection, rewind, native persistence, CTest and recompiler tests. |
| Combined normal-window, normal-audio owner playtest | Human approval pending | Prepared from the qualified game host. No final integration or original-worktree changes. |

## Candidate ledger

| Candidate | Outcome | Evidence |
| --- | --- | --- |
| Prior VIP row/source batching (`abc12ab`, separate existing worktree) | Rejected historically | Prior whole-runtime pair: -2.36% FPS, 0.59% process CPU reduction, foreign compilers present. Do not promote or silently fold into this investigation. |
| Draw-local affine map lookup reuse, persistent CHR/BG/object decode or display-list caches | Unverified / not selected | Reviewed existing aligned-row/object reuse and live writable maps. Profile did not justify added retained state/invalidation machinery. No general-game win claimed. |
| Immutable adaptive replay snapshots | Unverified / not selected | Large repeated snapshots exist only with tracking; no feature toggles or assumed SNES transfer. Existing implementation retained. |
| Stock presentation palette resolution | Accuracy qualified; standalone low-noise performance unverified | Call-local 96 x 4 table, sampled afresh per eye/call. No dirty flags, cross-eye cache or guest timing change. Both pairs reduce CPU, but A/A 4.59% exceeds the predeclared 3% quiet-control gate. Retained in the review candidate; not promoted as a proven general gain. |
| Single-allocation snapshot payload + private history CRC elimination | Accepted for combined candidate playtest | Exact counted payload replaces vector growth and a second complete copy. Private immutable ring entries skip redundant file CRC; external files require CRC and reject private flags. Combined depth-32 CPU reduction 53.31%, A/A 0.91%, both directions agree. Individual contributions within this snapshot change are not separated. |
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

`measure-fixed-headroom/results.json` records the complete predeclared series:
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
Final debug/UI game host SHA256:
`83cf72cf740e7c5e28e2c07d308458f41cc6e1d1f551abcbb52279214120c011`.
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

## Automated qualification evidence

| Evidence directory/log | Result and scope |
| --- | --- |
| `reference-final-driving` | PASS 39 checkpoints / 4,000 frames / 273 byte audits; independent Beetle CPU, full memory/device state, both eye images, presented output and stereo PCM. |
| `reference-current-mario` | PASS 15 checkpoints / 1,000 frames / 105 byte audits; same ten planes and current generated code. |
| `reference-final-mario`, `reference-source/stale-mario` | Retained failed run with obsolete consumer-generated code. Rebuilt using the unchanged current recompiler; no core fix or original corpus replacement. |
| `lifecycle-midframe-stock`, `lifecycle-midframe-color` | PASS 33 checks each; current game-host lifecycle, both eyes and source/provenance bytes, active color mod separately; mid-frame dispatch, replay PCM, all four framebuffer buffers, aliased CHR, reset, repeated presentation, rewind, feature/build/content/CRC rejection, atomic save failure. |
| `native-save` | PASS seven checks: exact load/save, old execution SRAM restore, newer native progress backed up, invalid native input rejected unchanged. Only isolated agent-owned files. |
| `final-ctest.log` | PASS 6/6: ROM patches, input, controller, interpreter bridge, VIP sources/viewport/brightness, mod runtime. |
| `final-recompiler-tests.log` | PASS 84; 5 optional oracle tests skipped explicitly (89 total). |
| `final-cli.log`, `final-route-adapter.log` | Malformed finite workloads rejected; route conversion checks pass. |
| `msvc-build.log`, `msvc-smoke.log`, `lifecycle-msvc` | Separate MSVC x64, SDL-free portable interpreter host builds/runs; PASS 33 lifecycle checks. This is not a non-Windows or cross-build compatibility claim. |

Native SRAM and old disposable execution snapshots have separate contracts.
All automated launches were headless or used dummy/zero-volume output. The
owner handoff uses a normal window, real audio driver and volume100 with the
combined local-history changes. Human controls/audio/feel and final integration
remain pending; network rollback, netplay and link cable behavior are absent.

Handoff: `playtest/Playtest.ps1` inside the evidence root. Stereo, color mod on,
32-frame local history, isolated SRAM/config/snapshot paths. The real Windows
audio driver is restored; software SDL presentation avoids driver shader-cache
output on C:. TEMP/TMP stay under the handoff directory. The executable is the
same qualified game host named above. Human checking should cover ordinary
controls and steering, music/SFX, F5/F8 during a turn, Backspace rewind/release,
both eyes, repeated loads and a reset/close/relaunch. No native owner save is used.

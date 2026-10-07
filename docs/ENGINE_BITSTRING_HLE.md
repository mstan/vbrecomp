# Shared-engine bit-string experiment

Owner: central issue `beads-cai4`. Baseline: `794a200f4001cb8314955794d2726ae1ea3f5123`.

The build-fixed `VBRECOMP_BITSTRING_IMPL=LLE|HLE` defaults to LLE. Both native
translations and the interpreter call the same instruction service. The HLE
replaces logical bit-string operations 8..15 with native masked word operations
and completes the instruction in one call. Search/invalid operations retain
the old floor. This is an engine service replacement, independent of guest
function identities or cartridge-specific patches.

Completed results preserve GPR/PC/cache state, source/destination overlap,
callback read/write sequence, and aggregate cycle charges. The focused
model includes the one-cycle instruction charge of each removed logical
redispatch; HLE charges it internally after the first word. Host speed must
not come from advancing guest frame time with fewer charged instructions.
The focused
contract compares 8,224 cases against the original implementation, including
all source/destination offsets, eight operators, wrapping/overlapping buffers,
cached-source entry, and long operations. Both selections also run the existing
interpreter semantics tests with their respective instruction boundary contract.

Timing differs: LLE returns after each destination word, leaving PC on the
instruction until completion; HLE returns only on complete logical operation.
`main.cpp` checks IRQs before dispatch and synchronizes devices afterward.
Generated/fallback dispatch checks `cycle_deadline` between execution units.
Timer/input register accesses and VSU writes can synchronize individual devices
inside bus callbacks; VIP events and IRQ acceptance still wait for dispatch.
An HLE operation can therefore cross an event deadline and defer delivery.
The focused completed-state test does **not** prove IRQ equivalence, arbitrary
MMIO bit-string behavior, gameplay completion, or whole-workload speedup.
Large guest lengths also increase the interval before the host regains control.
Keep HLE opt-in until actual routes establish an acceptable supported scope.

## Opportunity ranking

| Rank | Category | Evidence / cost | Disposition |
|---|---|---|---|
| 1 | CPU / memory fetch | Bounded interpreter-floor profile: `vb_read16` 164 samples and interpreter bodies 173 samples out of 479 mapped samples; no AOT generalization | Next investigation: interpreter fetch/decode/service batching at an understood common engine boundary |
| 2 | Graphics | `vip.c` block drawing and affine per-pixel sampling; normal background already has an eight-pixel path; `vb_vip_tick` 23/479 mapped samples | Profile block/affine workload before another replacement; tick samples are exclusive, not an inclusive graphics budget |
| 3 | Scheduling / external host | 63.712% samples outside the executable remain unresolved; deliberate 1ms HALT sleep in normal host | Resolve external cost before claiming comprehensive ranking; benchmark removes known host sleep identically |
| 4 | Logical memory service | Per-bit inner loop and destination-word redispatch replaced; zero calls on both measured gameplay routes | Retain opt-in service HLE; no material whole-game benefit established on these routes |
| 5 | Audio | `vsu.c` channel/event loops and `Blip_Buffer.c`; observed VSU functions 5/479 mapped samples | Low measured exclusive attribution on this interpreter floor; preserve audible output/event contract |
| 6 | DMA / bulk transfer | No independent DMA firmware service identified; logical bit-string is the relevant guest-visible bulk operation | Cover via bit-string experiment, no speculative DMA abstraction |
| 7 | Firmware | No external coprocessor firmware execution service identified in this runtime | No firmware candidate |

## Reproduction and qualification

Configure two Release builds with identical compiler/debug settings and
`-DVBRECOMP_BITSTRING_IMPL=LLE` or `HLE`, then build and run CTest. The standalone
contract build uses `cmake -S runtime/tests/bitstring -B build/bitstring-contract`.

`vb-runtime --rom <private-ROM> --headless --no-save --execution interpreter
--benchmark N --benchmark-route <route>` runs a finite uncapped engine workload.
The optional text route contains decimal `frame-count pad-mask` pairs totaling
exactly N. Timing starts after initialization and stops before hashes/cleanup;
ordinary host idle/presentation sleeps are excluded identically from both arms.
The record reports fixed implementation, actual frame count, elapsed seconds,
cycles, PC, and WRAM SHA-256. These metadata alone do not prove device/audio/video
equivalence. Use `tools/cosim.py` with both binaries and a real gameplay route to
compare CPU, memory, devices, both eyes and audio at milestones. Preserve ROM,
generated code and route artifacts privately.

`VBRECOMP_BITSTRING_DIAGNOSTICS=ON` enables process-lifetime logical-service call,
bit and completion counters for a separate coverage run. It defaults OFF and
must remain OFF for throughput: a per-word LLE counter and atomic HLE counter
would otherwise add different amounts of host work. Zero counter values in an
OFF build are not coverage evidence. `tools/cosim.py compare --runtime <LLE>
--runtime-b <HLE> --a-mode interpreter --b-mode interpreter ...` compares actual
separate builds without changing implementation selection at runtime.

Required remaining gates: bounded real gameplay against a functioning LLE
floor, inspect IRQ/device differences if any, serial order-balanced whole-route
throughput with binary/ROM hashes and process census, and explicit no-softlock
transition evidence. Independent Beetle remains valuable as a third reference;
a floor/HLE match alone cannot discover a shared emulator error.

## Bounded native Windows result

Both LLE and HLE generic runtime builds passed 8/8 root CTests, including the
completed-operation differential and malformed finite-workload arguments.
The generic interpreter floor and HLE passed the existing ZeroRacers
4,000-frame driving/pause route at 39 milestones and 273 byte audits across
CPU, WRAM/SRAM/VRAM, VIP/devices, both eyes/presented output, and audio.
Both processes executed 888,519,839 interpreted instructions. The final image
shows live race HUD and vehicle damage/reverse state; race finish is not proven.
These builds contain no cartridge-specific HLE or spoofed state transitions.

A separate coverage-only instrumented LLE binary then reported **zero logical
bit-string calls/bits/completions** on that same route and on the existing
1,000-frame MarioTennis regression route. The binary hash, unconditional
instrumentation identity, input conversions and raw logs are preserved under
`build/qualification/census/`. The initial exact comparison is under
`build/qualification/zero-racers/report.json` with both binary/ROM hashes.

Those cold routes are useful regression evidence but do not qualify this
replacement's gameplay timing or performance. Five throughput pairs on an
unexercised service would supply no meaningful HLE benefit evidence, so none
were run. No synthetic-only speed claim supports promotion. Diagnostic counters
are compiled out of final throughput/profile builds.

**Disposition: LLE remains default; logical HLE remains an opt-in draft.**
Interpreter fetch/decode, VIP graphics and VSU/audio are further shared-engine candidates;
the two measured routes show that logical bit-string cost is zero there,
despite its promising static per-bit implementation. No whole-game HLE speedup
or default-on qualification is claimed by this document.

The reviewed Windows main-thread sampler then captured the same finite
ZeroRacers route on the final LLE **interpreter** floor with diagnostics OFF.
It produced 1,320 samples, zero sampling errors and child exit 0. Matching GNU
`nm` symbols and PE preferred image base `0x140000000` corrected ASLR.
**841 samples (63.712%) were outside the executable and remain unresolved.**
Only 479 samples support the mapped ranking: ROM/bus `vb_read16` 164, interpreter
step bodies 173, dispatch 28, VIP tick 23 and VSU functions 5. These are exclusive
nearest-symbol samples with no inline stacks or inclusive caller attribution;
they are not a comprehensive host-cost profile or a shipped AOT workload.
No bit-string service sample was observed. The unresolved external majority
must stay prominent rather than being redistributed to graphics or CPU by guess.

Exact sampled binary, binary/ROM identity, `samples.csv`, matching `symbols.txt`,
`attribution.json`, and child log remain private in
`build/qualification/host-profile/`. This single bounded profile motivates
common interpreter fetch/decode service investigation and better external-code
attribution before another subsystem rewrite. It is not throughput evidence.

## Representative workload pool

Use these three existing routes for bounded hypothesis discovery, reusing
correctness evidence before fresh sampling. The generic interpreter profile
above, with **63.712% unresolved external samples**, is inadequate for ranking
production AOT costs. Its fetch/decode ranking must not become a production
optimization priority without production evidence.

This pool is not an automatic matrix. The shared six-system pass permits at
most six new captures total, one configuration per selected game, reusing
existing evidence first; it is not six captures per system.

| Workload | Existing local build and route | Reused evidence / production cost gap |
|---|---|---|
| ZeroRacers | `_wt-zero-racers/build-release/vbrecomp/runtime/ZeroRacersVirtualBoyRecomp.exe`; `tests/race-driving-route.json`, 4,000 frames | Release, CPU hooks/debug tools OFF. `validation/driving-release/report.json` passed 39 milestones/273 byte audits. Hybrid: 888,495,917 native instructions and 23,922 interpreted/fallback instructions; near-AOT, not zero-fallback. Production subsystem costs unknown. |
| Wario Land | `WarioLandVirtualBoyRecomp/build/vbrecomp/runtime/WarioLandVirtualBoyRecomp.exe`; `tests/gameplay-route.json`, 2,730 frames | Movement/jump/attack/turn/hazard route. Historical native-only Beetle comparison matched ten planes/189 audits, 496,082,918 native instructions, zero fallback. Available Release build has CPU hooks/debug tools ON; identify that overhead in any profile. No campaign completion or production cost ranking. |
| SD Gundam | `_wt-sd-gundam/build-release/vbrecomp/runtime/SDGundamDimensionWarVirtualBoyRecomp.exe`; `tests/first-mission-route.json`, 4,500 frames | Production Release uses normal generated optimization, hooks/debug tools OFF. Historical `validation/japanese-native-callback/report.json` passed 40 milestones/280 audits, 914,443,855 native instructions, zero fallback, using an O1 fast-build configuration. Do not equate that validated binary with the production binary. Opening mission/unit-action menu only; combat/completion and production costs unqualified. |

ZeroRacers is the next single useful production capture: a debug-free near-AOT
build and recorded driving route already exist. Older title runners lack this
branch's finite `--benchmark` interface; prepare a bounded route-driven sampler
adapter before launching. Preserve matching binary/symbol identity and report
unresolved samples explicitly. No production VB capture was added in this pass.
The earlier SD Gundam `japanese-native-final/report.json` failed with a socket
reset and is not passing evidence; the callback report is the passing record.
Cold logical-bit-string findings on generic ZeroRacers and MarioTennis remain
regression/coverage evidence, not production AOT cost measurements.

## End-to-end engine strategy

The first task is a finite production-native sampling adapter, not another BSU
rewrite: drive the existing route with actual controller input, stop at a guest
frame/event boundary, and preserve normal title runtime semantics. Use one
short production capture per ready route with matching symbols and all-thread
CPU accounting. Identify framework/pacing tails and unresolved external code
rather than assigning them to a guessed device. Stop after this bounded
three-title pass and choose one common engine service, or explicitly report
that the measurements cannot select a material candidate. No automatic extra
profiles or 90-leg matrix follow an inconclusive result.

| Title / role | Concrete route and comparison milestones | Production floor readiness and caller contract |
|---|---|---|
| ZeroRacers / primary | `tests/race-driving-route.json`, 4,000 frames; reuse 39 milestone/273 audit route including menu-to-live-race progression and driving/pause state. Observe race HUD/player movement, both-eye presentation and pause/return responsiveness; final race damage/reverse HUD is not a finish. | Debug-free production near-AOT binary exists, but finite adapter is missing. Preserve/report its small fallback fraction. Match controller-visible race progress, both-eye VIP output and VSU continuity against the functioning floor; existing interpreter comparisons do not substitute for native qualification. |
| Wario Land / companion | `tests/gameplay-route.json`: first-stage entry 2,120, walk 2,220, jump 2,250, attack 2,360, second jump 2,480, turn 2,610, final idle 2,730. Use the first-stage interval for active-game cost, retaining startup/transition checks. | Native-only Beetle floor is documented; available Release has debug/CPU hooks ON. Prepare a pinned production build with hooks OFF and revalidate this route before comparing costs. Preserve movement/jump/attack/hazard behavior, stereo output, audio and input response; no campaign claim. |
| SD Gundam / companion | `tests/first-mission-route.json`, 4,500 frames: safety/alignment/auto-pause, opening Japanese mission map and unit-action menu. Reuse 40 milestones/280 audits, checking tactical cursor/menu progress and map presentation. Combat and mission completion are outside this route. | Debug-free normally optimized production binary exists; the passing native oracle report used historical O1 fast-build. Revalidate the production floor and bounded adapter before timing. Preserve menu/cursor/unit-action behavior, both-eye display, sound and transition responsiveness. |

The decision point follows those captures. Choose VIP row/block rendering if
production native samples and eligible active-game coverage make it the largest
plausibly removable shared cost; choose VSU block synthesis if audio is actually
hot; choose bus/scheduler service batching only when native caller attribution
shows material avoidable work. Declare the chosen service's input/output ABI,
completion/interrupt observations relevant to these callers, supported operation
scope and allowed minor differences before code. Preserve the working LLE as a
fixed build alternative. The existing logical BSU draft is cold on tested
routes: retain it opt-in or retire it from this critical path. Do not force an
owner to test an unused BSU path or infer savings from synthetic-only results.

For the selected service, compare per-game caller behavior at the table's
milestones and use independent Beetle evidence where applicable. Require exact
outputs where the candidate promises exactness; assess any explicitly permitted
small image/audio/timing differences practically against progress and normal
input response. Complete internal-state or IRQ-trace identity is not a universal
gate. A service that changes operation timing still needs concrete tests for
its affected caller observations; do not hide a race/pause/menu stall behind a
pixel match. Diagnostics establish eligible coverage separately from timing.

Declare a material whole-work reduction target before implementation (10% is a
planning target, not a universal threshold). On the same useful active-game work,
measure all-thread CPU and wall/framework/pacing costs: primary ZeroRacers ABBA
(two balanced pairs), Wario and SD Gundam one A/B each. Stop at that screen;
park noisy, cold or immaterial outcomes rather than automatically repeating.
Only a candidate that beats measured noise, meets its declared useful-work goal
and passes the three scoped caller contracts reaches the owner handoff.

Provide the owner a ready normal-paced ZeroRacers production HLE game, standard
controls and ROM launch instructions, plus a fixed LLE build opt-out. The owner
plays real driving and pause/return interactions and judges responsiveness,
stereo presentation and audio feel. Passing that final feel check permits merge
and HLE default only for the qualified platform/title/configuration scope, then
issue closure with measurements and limitations. Broader defaults require
broader evidence; the menu-only SD Gundam route cannot prove combat or completion.

## Measurement, decision and delivery protocol

Owner completion rule: establish a material game-workload gain and automated
compatibility, then deliver the final playable build for the owner's feel check.
After that check passes, integrate the prepared default change and close the
scoped work. Exhaustive game coverage and completed campaigns are not additional
completion requirements.

1. **Pin the workload and floor.** Use the three games and concrete routes above.
   Build LLE and HLE from the same title/framework revisions, compiler/options,
   ROM/firmware identities, presentation/audio settings and initial game state;
   only the selected implementation differs. Keep the replaced LLE service
   runnable. An old executable is discovery evidence, not a mismatched control.
   Use native game saves or replayed inputs when private savestates cannot cross
   builds. First resolve the named route/build gaps; do not perfect unrelated
   hardware before replacing a functioning operation.
   Verify that companion routes actually exercise the replacement; an unaffected
   title is a regression control, not evidence for that HLE service. If the
   chosen service changes, replace an unsuitable companion in the three-title
   set instead of accumulating extra games or claiming unexercised coverage.
2. **Attribute only what is missing.** Reuse suitable profiles and collect at
   most one new active-workload attribution capture per selected game in this
   implementation round. Identify the intended service's eligible dynamic work.
   Include worker threads and external modules or report them unresolved; a
   main-thread symbol histogram cannot supply a whole-process cost percentage.
   Capture diagnostics separately from performance. End discovery when there
   is enough evidence to select a useful service, not when every subsystem has
   a profile. The earlier six-launch discovery cap applied to that completed
   pass, not to the whole implementation/qualification program.
3. **Choose one replacement.** Record its caller ABI, inputs, outputs, observable
   side effects, supported operation scope, permitted tiny differences, expected
   cost removed, and candidate-specific useful gain before coding. Implement a
   shared service with build-time LLE/HLE selection and explicit build identity.
   Do not stack several speculative replacements into the same comparison.
4. **Measure equivalent active play.** Delimit a fixed gameplay window by guest
   frames and meaningful game events, excluding boot, warmup and teardown.
   Choose enough active work to dominate measurement granularity once, then keep
   it fixed. Report total process CPU milliseconds per guest frame (all threads),
   critical-path frame work, median/p95 frame time and missed presentation/audio
   deadlines where available. Record peak memory and code size, since constrained
   targets matter. Preserve normal renderer and audio production; a benchmark
   that omits presentation/audio is a core-only diagnostic, not end-to-end proof.
   The owner selected Windows first and authorized uncapping for useful
   measurements. Prefer a finite uncapped comparison where it preserves the
   same game, render and audio-synthesis work. Remove host frame-delay/VSync
   waits only in isolated benchmark configuration; do not change the guest
   timing model, resolution, effects, audio workload or HLE coverage between
   builds. Report uncapped FPS and milliseconds/frame alongside total CPU/frame,
   and verify completed render/audio work and game progress rather than trusting
   a frame counter alone. A legacy benchmark that skips rendering/presentation
   or audio remains core-only evidence; use a complete paced CPU/frame comparison
   until that benchmark path can exercise equivalent work. Normal capped play
   can show reduced CPU/frame even when FPS stays unchanged. Measure GPU
   completion/queue cost when work moves there; a shorter submission call alone
   is not a win. Keep the final owner-playtest package normally paced.
5. **Use a fixed comparison budget.** The primary game gets LLE/HLE/HLE/LLE:
   two order-balanced pairs, four measured executions. Each of the two companion
   games gets one LLE/HLE pair, two executions each. That is eight measured runs
   per candidate on one declared host/configuration, not a Cartesian matrix.
   Reuse their progression telemetry and final outputs; take expensive milestone
   captures outside timing, and use isolated LLE/HLE fixtures for detailed
   contracts. Do not automatically add separate full campaigns or trace runs.
   Keep team builds/profiling out of the timed window, record host load/power/
   thermal conditions, and preserve every result. A noisy or contradictory result
   stops that screen; fix an identified condition before a bounded replacement
   measurement. Never repeat until a passing subset appears.
6. **Decide from useful gain and compatibility.** Report both paired percentage
   and absolute savings, with the observed pair spread. About 10% lower whole
   active-workload CPU time is a planning aim, not a universal acceptance rule.
   A candidate may instead solve a declared frame-budget or stutter problem.
   Both primary pairs must show a clear consistent useful improvement beyond
   observed noise; two pairs are not a formal confidence interval. Companion
   single pairs screen for large regressions, not proof of zero performance
   change. Explain any apparent regression before broadening defaults. Exact
   promises require exact outputs; permitted approximations use a declared
   practical image/audio/result comparison. Check input, audio, progression,
   affected completion/IRQ consumers, transitions and relevant pause/reset/save
   behavior. No crash, softlock, stale buffer, lost completion or save corruption
   passes. A huge isolated kernel ratio cannot substitute for this decision.
7. **Hand off the actual finished candidate.** Provide the named primary game as
   a ready-to-launch normal-paced HLE package, an LLE comparison build, isolated
   save/checkpoint setup, launch instructions and checksums/build identity. Include
   a short before/after report, companion results and any tiny known differences.
   Prepare the intended default-selection/integration change in the draft PR so
   the owner tests the package intended to ship. Ask the owner to play normally
   and assess response, motion/collision, camera/scrolling, stereo where relevant,
   audio rhythm and continued progression. There is no prescribed full-campaign
   completion or multi-game human test matrix. Owner rejection reopens the
   affected behavior; fix and recheck that change before another handoff.
8. **Finish the scoped delivery.** After owner acceptance, integrate the reviewed
   candidate, make HLE the default for the supported titles/platform/service,
   retain a documented build-time LLE opt-out, and record the measured and manual
   evidence before closing the issue. Do not add unrelated qualification gates
   after the agreed playtest. If the replacement cannot deliver material gain,
   preserve its branch and draft PR with results, explain why, and choose a new
   boundary deliberately; an unsuccessful experiment is not a completed system.

The owner selected **Windows first; port measurements later**. Windows x64 is
therefore the initial implementation, measurement, final-playtest and default
scope. After automated checks, material gain and the owner's normal-paced feel
approval, finish that Windows delivery; a mobile/Xbox port is not a new gate
before closure. Later port work carries the winning candidate and relevant
routes to the chosen target and measures there before claiming target savings.
Do not multiply all hosts into the Windows discovery/comparison matrix.

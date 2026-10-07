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

### Native VIP candidate, 2026-10-06 (implementation in progress)

The finite native ZeroRacers adapter is now prepared: the public
`tools/prepare_benchmark_route.py` converts the existing 4,000-frame driving
route without guest-memory writes. The Release floor uses current framework
code, CPU hooks/debug tools OFF, BSU diagnostics OFF and BSU LLE. It remains
hybrid: 888,495,917 native instructions and 23,922 fallback instructions.

The single main-thread native capture collected 274 samples: 92 generated
cartridge (33.58%), 61 VIP (22.26%), 36 bus/memory (13.14%), 26 native dispatch
(9.49%), 17 WRAM fingerprinting (6.20%), one VSU (0.36%) and 13 unresolved
(4.74%). `vb_vip_tick` accounts for 56 (20.44%). Thirteen samples land inside
the attribution row-copy loop, where LLE calls the tracking predicate once per
pixel even with tracking disabled. This supports a shared VIP draw-service
candidate rather than further cold-BSU work.

Fixed `VBRECOMP_VIP_IMPL=LLE|BATCHED` retains LLE by default. BATCHED copies
contiguous attribution/source rows and prevents heavy block drawing from being
inlined into the frequent tick front end. The synchronous tracking flag is
stable within drawing. Guest framebuffer reads, status/register behavior,
drawing deadlines and IRQ service remain the caller contract; device timing is
not replaced. This is an exact native renderer optimization, separate from
the bitstring HLE draft. Target: roughly 10% useful whole-runtime reduction.

Capture artifacts are private under `build/qualification/native-zero-racers`;
matching `samples.csv`, `symbols.txt`, `preferred-base.txt`, `attribution.json`
and child log are retained. Sampling includes startup and fingerprint work,
perturbs execution, excludes SDL presentation/device delivery and observes only
the main thread. Foreign compiler activity was recorded. Its printed FPS is
not a throughput result or isolated-host claim.

`--measure-runtime N --benchmark-route PATH` provides an uncapped finite SDL
path retaining conversion/upload/presentation, VSU synthesis and device audio.
It reports FPS, process CPU, presented frames and synthesized audio frames;
the existing bounded audio ring may overwrite samples when uncapped. Normal
human launches remain paced. No gain, visual acceptance or default promotion
is claimed before the matched production run and owner playcheck. Both native
Release arms now build successfully. Existing VIP/source/viewport checks pass
for both choices; route-adapter checks and malformed finite CLI checks pass.

The first attempted 4,000-frame full-runtime LLE launch timed out at 120 seconds;
BATCHED never ran, so it supplied no performance evidence. Unlike headless
benchmarking, the initial finite SDL mode still entered the default shared
launcher despite a supplied ROM. Finite workloads now bypass that waiting UI
while retaining runtime SDL rendering/audio; both arms rebuilt successfully.
The private pair harness streams logs and saves failed/timeout metadata. The
old timeout output was discarded and cannot be reconstructed; its failure is
recorded under `build/qualification/vip-runtime-initial`.

The corrected 60-frame SDL smoke, without `--no-launcher`, completed exit 0:
59 presentations, 52,632 synthesized audio frames and active native execution.
The single corrected 4,000-frame production runtime pair then completed:

| VIP build | FPS | Runtime wall seconds | All-thread process CPU seconds |
|---|---:|---:|---:|
| LLE | 564.123 | 7.090654 | 8.000000 |
| BATCHED | 550.790 | 7.262302 | 7.953125 |

Both performed 3,999 presentations and synthesized 3,508,807 audio frames;
native/fallback counts matched the production route above. FPS changed
**-2.36%**, with only **0.59%** process-CPU reduction: no material whole-runtime
win. Foreign compiler census was 4→3 processes for LLE and 3→3 for BATCHED;
this is not an isolated-host claim. The candidate stays draft/default LLE.
No reverse pair, companion compilation/timing or owner playcheck is justified
by this negligible result. Private raw JSON/logs and binary hashes are under
`build/qualification/vip-runtime-corrected`; smoke under `vip-runtime-smoke`.

Companion preparation preserves generated files privately, without regeneration:

| Game | Pinned isolated title / existing floor | Prepared route and configuration |
|---|---|---|
| Wario Land | `_wt-vip-native-wario`, title `e0bd7215a2ea3e40115b60383daeeee8036f4c9a`; historical native framework `1dac7603074cf8d69f7ce96ff84936ee9d2257f4` | Original `tests/gameplay-route.json`, 2,730 frames; current-framework Release LLE/BATCHED configured, hooks/debug OFF. Existing `validation/native-oracle-01/report.json` passed 27 checkpoints/189 audits with 496,082,918 native instructions, zero fallback. |
| SD Gundam | `_wt-vip-native-sd-gundam`, title `4f2cee37d1fd69576a8ce9f595bd6d52768649c3`; historical native framework `794a200f4001cb8314955794d2726ae1ea3f5123` | Original `tests/first-mission-route.json`, 4,500 frames; current-framework Release LLE/BATCHED configured, hooks/debug OFF, fast-build OFF. Existing Japanese native callback report passed 40 checkpoints/280 audits, 914,443,855 native instructions, zero fallback, with historical O1 generated optimization. |

Both companions reuse the same UI pin as ZeroRacers. Their configured current
production binaries are not yet built or qualified; historical reports do not
assert current binary identity. Clean companion compilation and any pairs are
parked after ZeroRacers failed to show material runtime gain. Matching generated
hashes and pins are recorded in private `companion-identity.json` beside the
native profile. This is preparation, not an additional profile matrix.

Windows is the first supported target. Use three actual games, one production
configuration per game, and maintain a functioning LLE build with the same
caller ABI. The owner judges practical appearance and playability; prospective
validation does not require pixel-perfect old/new images or internal-state
identity. Historical exact comparisons above remain completed evidence.

### First task and implementation decision

The prepared finite production-native route adapter feeds actual controller
input, stops at the selected guest frame/event boundary and retains normal title
runtime semantics. The prior generic interpreter capture, with 63.712%
unresolved external samples, cannot rank production AOT engine costs.

The single native ZeroRacers pass above selects VIP row/block work. Do not
extend the profile matrix before testing this concrete candidate. Keep startup,
framework work and unresolved external code explicit; companion routes become
useful once a material primary gain supports further qualification.

Choose VIP row/block rendering if it is the largest removable native cost;
choose VSU block synthesis if audio is actually hot; choose bus/scheduler
batching only when native caller evidence supports meaningful savings.
Declare the service's input/output ABI, supported operations and concrete
caller observations before code. Keep LLE as a fixed build alternative.
The logical BSU draft is cold on tested routes: preserve it opt-in or retire it
from this critical path. It is not ready for an owner playtest or promotion.

### Selected games and floor readiness

| Game | Actual route / useful observations | Production readiness |
|---|---|---|
| ZeroRacers | `tests/race-driving-route.json`, 4,000 frames: menus, live driving and pause state. | Debug-free production near-AOT LLE/BATCHED builds and finite adapter ready. Preserve/report its small fallback fraction. Primary runtime gain candidate. |
| Wario Land | `tests/gameplay-route.json`, 2,730 frames: first-stage entry 2,120, walk/jump/attack, turn 2,610, idle 2,730. | Native-only Beetle floor is documented; available Release has hooks/debug tools ON. Prepare a pinned production build with them OFF and confirm basic route operation before timing. |
| SD Gundam | `tests/first-mission-route.json`, 4,500 frames: opening Japanese mission map, cursor and unit-action menu. | Debug-free normally optimized production binary exists; historical passing oracle report used O1 fast-build. Confirm the production route works rather than assuming binary identity. |

The caller contract is ordinary input response and progress, working both-eye
VIP presentation and VSU sound for these routes. Operation timing changes need
focused checks only for affected caller observations. Reuse relevant existing
runtime/device tests and independent reference evidence. Add a test only for a
concrete uncovered correctness risk. Complete IRQ/internal-state trace identity,
automated screenshot/state/audio sweeps and campaign/lifecycle gates are not
universal requirements for this prospective task.

### Measurement and decision

Before code, declare the chosen boundary and material gain target; 10%
whole-work reduction is a planning target, not a universal threshold. Start with
one matched uninstrumented LLE/HLE performance pair per game and report FPS and
percentage gain for equivalent finite inputs/useful work. Uncapped Windows
measurements are allowed; coverage diagnostics remain separate from timing.

Repeat a reversed pair only if observed noise or ambiguity blocks a conclusion.
There is no mandatory eight-run quota, automatic parameter matrix or indefinite
profiling loop. A cold service, noisy result or immaterial gain parks the
candidate. Synthetic-only improvement does not establish whole-game benefit,
and skipped useful work does not count as faster service execution.

### Visual check and owner handoff

For the new replacement, make one basic current-implementation visual sanity
glance: does the game look right and avoid garbled output? No old/new baseline
image matching is required. Escalate only for a concrete defect found in that
glance, focused testing or owner feedback.

Once objective gain and focused correctness pass, prepare normal-paced Windows
HLE builds of all three selected games with standard controls, ROM launch
instructions and a fixed LLE alternative. Launch every ready selected game for
the owner without asking again for launch permission; ask whether each looks
and plays right. Cold BSU/interpreter-only artifacts are not this handoff, and
uncapped benchmark mode is not the human playcheck.

Positive owner feedback plus material objective gain permits merge and HLE
default for the supported Windows scope, retaining the fixed LLE build opt-out.
Record the supported games/configuration and evidence, then close the owning
issue. A reported defect triggers targeted investigation of that behavior;
other platforms remain outside this initial scope.

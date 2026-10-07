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

Windows is the first supported target. Use three actual games, one production
configuration per game, and maintain a functioning LLE build with the same
caller ABI. The owner judges practical appearance and playability; prospective
validation does not require pixel-perfect old/new images or internal-state
identity. Historical exact comparisons above remain completed evidence.

### First task and implementation decision

First add a finite production-native route adapter: feed actual controller
input, stop at the selected guest frame/event boundary and retain normal title
runtime semantics. The prior generic interpreter capture, with 63.712%
unresolved external samples, cannot rank production AOT engine costs.

Make a bounded production cost pass over the ready routes with matching binary
symbols and relevant CPU/wall accounting. Separate startup/pacing/framework
work and unresolved external code rather than assigning them to a guessed
device. Stop after the three-title pass and select one material common service,
or explicitly report that the measurements do not support a candidate.

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
| ZeroRacers | `tests/race-driving-route.json`, 4,000 frames: menus, live driving and pause state. | Debug-free production near-AOT binary exists; finite adapter is missing. Preserve/report its small fallback fraction. Primary native profiling candidate. |
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

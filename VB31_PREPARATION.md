# VB31 structural interpreter-span revision

This is the final bounded structural experiment after `c688a331`. That earlier
candidate failed its native Wario control despite an interpreter-profile benefit;
it is not promoted. Existing evidence remains retained outside this worktree.
The new revision is **not accepted**: root controls real game qualification and
matched per-game timing after source/component/generic compatibility checks.

`VBRECOMP_INTERPRETER_BATCH=OFF` is the global and header default. It compiles
the original public `vb_interpreter_step` body and original generic/native
dispatch behavior; the new inline body, wrappers and span functions are absent.
The ISA body has one source definition behind a gated declaration, not two
implementations to maintain. Explicit ON is the standalone generic build profile.

The generic `NO_GAME_LINKED` host now batches its real default HYBRID fallback
as well as forced interpretation. Every iteration preserves live fetches,
observers, debugger/simulation callbacks, halt/yield/caller/budget/deadline exits,
and execution-mode changes. HYBRID charges budget and fallback coverage only
after a retired instruction; forced interpretation keeps its original pre-step
budget charge. Mode changes can cross between these paths at the next iteration.
Native-only misses retain their original failure. Generated native routing stays
on the original OFF path in the safe consumer profile.

Supported profiles, using the ordinary existing build pipeline:

```sh
# Native consumer/game builds: default OFF, or make it explicit in their CMake job.
cmake -S . -B build-native -DVBRECOMP_INTERPRETER_BATCH=OFF

# Standalone generic host: empty generated module plus explicit ON.
cmake -S . -B build-generic -DVBRECOMP_GAME= -DVBRECOMP_INTERPRETER_BATCH=ON
cmake --build build-generic --target vb-runtime --parallel 1
python tools/qa/generate_interpreter_workload.py --out diagnostic --runtime build-generic/runtime/vb-runtime.exe
```

The original public diagnostic defaults to omitting `--execution`, exercising
the generic host's default zero-valued HYBRID mode. Use `--execution interpreter`
on the generator to prepare its forced-profile control. Neither command launches
implicitly. The differential fixture covers direct forced spans and actual
generic public dispatch/call APIs in forced and default HYBRID modes, including
bidirectional observer mode changes and exact fallback-PC coverage. Its frozen
reference remains the untouched retained interpreter.

The earlier preparation below describes the original forced-only experiment;
this section supersedes its defaults, routing and pending-state statements.

Status: isolated prototype with component checks. No built-game qualification,
performance result, retention decision or merge approval exists for this item.

The generic `NO_GAME_LINKED` runtime and the public `--execution interpreter`
mode are supported workloads. Native Wario/Zero runs with zero interpreted
instructions cannot close this opportunity. Inspection also disproves the
initial native-lookup hypothesis for forced mode: the generated dispatcher
already checks interpreter selection before native lookup.

The concrete experiment keeps the existing interpreter instruction body inside
one shared loop in its own translation unit. This removes an external step
call for every forced-interpreter instruction. It introduces no decoded-code
cache and never removes live instruction/data-bus reads. Explicit single-step
and hybrid fallback APIs keep their caller contract. Generated dispatch and
the generic host call the span only in forced-interpreter mode; native routing
is unchanged.

Each iteration retains the original order: caller/halt/yield checks,
budget/deadline test, budget decrement, debugger/simulation callback, live fetch,
CPUHOOK, accounting, and instruction effects. The final instruction's original
deadline overshoot remains legal. Backend selection is checked again after each
instruction, so an observer can hand execution back to native/hybrid dispatch.
No hidden decode state needs save/load invalidation.

`VBRECOMP_INTERPRETER_BATCH` controls generated/generic routing and is included
in strict build identity. The internal inlining hint is guarded for GCC/Clang
and MSVC; other C11 compilers use an ordinary inline function under the same
interface. There is no assembly or new dependency.

Component coverage includes the frozen retained interpreter from
`9ffb83cafc7ffbd973da1a1318dc28c6df1f5889` (the reference `.inc` is verbatim),
CPU state, memory, observable live-bus state traces, code mutation, exact
budget/deadline/caller/halt/yield gates, debugger stepping/breakpoints, simulation
callbacks and execution-mode changes. Debug, production and CPUHOOK variants
each cover 12,000 differential cases. Long arithmetic/load/store spans exercise
more than 17,000 retired instructions in each variant. Existing generated
dispatch tests cover both native routing and batch switches. These are
functional component checks, not built-game speed measurements.

`tools/qa/generate_interpreter_workload.py --out OUTPUT --runtime RUNTIME`
prepares an original 64-KiB diagnostic ROM and an exact JSON job with
`--execution interpreter --benchmark 120 --no-save`. It never launches a process.
The diagnostic mixes arithmetic, WRAM load/store and looping control flow;
there are no third-party ROM bytes. Its job must be run under the normal
headroom reservation once a generic runtime is built. Synthetic diagnostic
timing cannot replace game-by-game results.

At the serial gate, build OFF/ON production and observer stages using the
supported consumer pipeline and preserve immutable source/binary/settings
identities. First obtain the smallest useful inclusive forced-interpreter
baseline. Compare OFF/ON on the same supported interpreter profile for each
representative game, with unchanged color/widescreen/rewind settings and the
same host/configuration per A/B pair. Keep a native-profile control and qualify
current-build save/load/rewind, device/IRQ boundaries, both eyes and PCM where
the changed profile needs them. Do not compare interpreter against native and
attribute that backend difference to this optimization. A baseline-equivalent
failure is tracked separately; reproducible individual-game regressions reject
the optimization. The shared-engine owning issue is `beads-vb.1.16`.

# Generated arithmetic regions

The emitter retains every instruction resume label and its original body. In
addition, runs of3–12 arithmetic instructions have a fast entry. Registers and
flags remain local within the run; cycles, pipeline state, PC, instruction count
and budget are materialized at its end. This is shared generator work and does
not depend on a title's ROM addresses or algorithms.

An eligible instruction has no bus access, callback, control transfer, exception
or interrupt-mask change. Division, extended operations, system registers and all
loads/stores remain on the instruction path. A region requires enough budget and
cycle headroom for its complete exact cost. The original checks execute near a
deadline, retaining the original final-instruction overshoot. An arbitrary resume
or branch into the interior also uses the original instruction labels.

Instruction stepping/breakpoints disable the fast entry while active. CPU-hook
builds always execute the individual instruction path. A dynamically enabled
simulation point anywhere inside a run also disables that entry; enabled points
outside the run do not. Registration and enable/disable happen on the guest
thread, and pure runs have no callback that could change the registry midway.

`VBRECOMP_NATIVE_REGIONS=OFF` is a build control for matched comparisons. The
choice is part of the strict state-build identity. Both paths are portable C11;
no CPU-specific features or host assembly are used.

The generated fixture compares optimized bodies against the original emitter
and independently against the interpreter. It varies arithmetic/register alias
patterns, every resume PC, budgets, exact/near/wrapped cycle boundaries, pipeline
state, dynamic observer/redirect hooks, debugger stepping/breakpoints and CPU-hook
records. Game-route correctness and production timing are separate gates.

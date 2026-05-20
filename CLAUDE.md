# vbrecomp — Rules

This file is the constitution for the Virtual Boy / NEC V810 static
recompiler. Read it at the start of every session before doing any work.

The structure of this file is adapted from `psxrecomp/CLAUDE.md` (v4).
Every rule that survived the PSX project survives here. PSX-specific
clauses (BIOS-first, dirty-RAM interpreter) are reframed for V810.

---

## 0. The architecture is locked

vbrecomp implements **static V810-to-C recompilation** of the cartridge
ROM, producing native C that links into the runtime as real compiled
functions.

There is **no V810 interpreter** in this project. Not as a fallback.
Not as a "temporary" measure. Not for "code we couldn't recompile yet".
If a function in the cartridge ROM cannot be recompiled, the recompiler
is wrong and must be fixed. The interpreter does not exist. Do not
write one.

There is **no HLE layer**. No `bios.c` synthesising what a routine
"would have produced". No C reimplementations of game functions. The
cartridge IS the recompiled C output of the ROM. If a routine
misbehaves, the answer is to fix the recompiler, or fix the hardware
simulation it touches via MMIO — never to write a C "shim" that
produces the answer the cartridge would have produced.

There are **no stubs**. A function is either fully implemented or it
aborts with a fatal error via `vb_stub_abort(...)`. `return 0;`,
`return 1;`, `cpu->gpr[10] = 1; return;` are all stubs. `// TODO`,
`// FIXME`, `// for now` are all stubs. Hand-delivering a frame
because the renderer isn't installed is a stub wearing a costume and
is the worst kind because it hides the missing integration.

If you find yourself wanting to violate any of the above three
paragraphs, **stop and re-read PLAN.md**.

---

## 1. Mario's Tennis is recompilation target #1; homebrew is a tool, not a phase

The plan was revised on 2026-05-19 to drop the homebrew-first phase.
Mario's Tennis is the smallest commercial cartridge (512 KB) and the
ROMs are already in `roms/`. The oracle backend (Beetle VB) plays
Mario's Tennis correctly out of the box, so we have a usable visual
reference from P2.5 onwards.

Homebrew test ROMs are still useful as small-scope decoder/lifter
test inputs (no need to recompile 192k instructions to find a bug),
but they are NOT the gate for moving forward. The gate is whether
*Mario's Tennis* behaves correctly at each phase, against the oracle.

If you find yourself wanting to "just test on Mario's Tennis early to
see what happens," that's exactly what P2.5/P3/P3.5 are for — they
each produce a partial Mario's Tennis result. Wait for the phase, do
the real work that gets you there.

---

## 2. Three sources of truth, in priority order

1. **V810 Architecture Manual** + the Virtual Boy Programmer's Manual
   (`docs/HARDWARE_NOTES.md` references). What the hardware is
   supposed to do. Check this FIRST.
2. **Ghidra** with the third-party V810/V830 SLEIGH module — for what
   the raw bytes are at a given address (static analysis of the
   cartridge ROM). Useful for labels, xrefs, block discovery,
   instruction validation. **Do not make Ghidra the source of truth
   for execution correctness.** SLEIGH bugs exist; we validate against
   the oracle.
3. **Beetle VB oracle** (`vb-beetle.exe`, separate process, TCP port
   4391) — for what real Virtual Boy hardware does at runtime.
   Cross-process comparison via TCP, never lockstep.

Use all three, never just one. Don't guess. Don't say "probably". If
you cannot answer a question from the manuals, Ghidra, or the Beetle
oracle, the answer is "I don't know yet" — not a confident guess.

---

## 3. No printf debugging. No log files. Ever.

If you need to inspect runtime state, **build a TCP debug server
command** for it. Past projects accumulated gigabytes of
`*_trace*.log` because previous sessions used `fprintf` for "just this
one thing". The rule is absolute: **no `fprintf(stderr, ...)` in
source code, ever, for any reason.** The single exception is the
banner printed by `vb_stub_abort()` immediately before `abort()` —
because that's a crash report, not a debugging aid.

All inspection goes through the TCP debug server on port **4390**
(runtime) or **4391** (oracle).

Always-on ring buffers (frame snapshots, fntrace, wtrace) cover
historical events. Probes QUERY the ring; they do NOT arm a trace,
run a workload, and hope. See the global "always-on ring buffer"
rule in `~/.claude/CLAUDE.md`.

---

## 4. Never modify generated code

The output of the recompiler — files in `generated/` — is a build
artifact. If the generated code is wrong, the fix is in the
recompiler source (`recompiler/v810/emitter.py` and friends), not
in the generated file.

After every recompiler change: regenerate, rebuild, run. An edit to
the recompiler is not "done" until those three steps succeed and a
deterministic smoke confirms the change. See `~/.claude/CLAUDE.md`
auto-memory: *"Recompiler changes need regen+rebuild+run before
commit"*.

---

## 5. Don't accept partial milestones

Phase completion requires the user-visible end state, not "I think it
should work now". Phase 5 is "Mario's Tennis title screen renders and
matches oracle for 5 seconds". Not "the recompiler emitted code that
probably draws the title". Not "the VIP command stream looks right
in the debug server". **The pixels appear on screen, or the phase is
not done.**

---

## 6. Session start checklist

At the start of every session, before any code change:

1. Read this file (`CLAUDE.md`).
2. Read `PLAN.md` to confirm what phase we are in and what the next
   concrete milestone is.
3. Verify `docs/HARDWARE_NOTES.md` and `docs/INSTRUCTION_STATUS.md`
   exist.
4. State out loud: "Static recomp. No interpreter. No HLE. No stubs.
   Homebrew first. Mario's Tennis only at Phase 5."

If any of these fail, do not proceed with the user's task — surface
the failure first.

---

## 7. Salvage from sibling projects — what's allowed and what's not

The runtime skeleton in `runtime/` is salvaged from `psxrecomp/runtime/`
because the core dispatch+ring+TCP pieces are platform-agnostic. The
Python recompiler pattern is salvaged from `snesrecomp/recompiler/v2/`
(decoder.py, ir.py shape). Attribution is in `LICENSE`.

What may NOT be salvaged:
- PSX MIPS decoder logic. V810 has its own opcode table, its own
  formats, its own flag semantics. Don't copy MIPS tables and edit.
- PSX HLE shims. None of them exist for VB. If you find yourself
  porting one, stop.
- PSX dirty-RAM interpreter. V810 does not (in mainstream games)
  install code at runtime the way the PSX BIOS did. If a specific
  game needs it, reinstate the pattern with a fresh implementation —
  do not pre-emptively port it.

---

## 8. Reference the right project for examples

vbrecomp is a sibling project to:

- **`psxrecomp/psxrecomp/`** — closest peer (RISC, runtime split, oracle)
- **`snesrecomp/snesrecomp/`** — Python recompiler pattern (v2 IR)
- **`nesrecomp/nesrecomp/`** — TCP harness + reverse debugger tiers
- **`segagenesisrecomp/SonicTheHedgehogRecomp/`** — coroutine/fiber dispatch model

When you need "how does a recomp project handle X?", read those.

---

## 9. Memory and prior session context

Auto-memory continues to work across sessions. Existing memories about
printf rules, no-stubs, regen-rebuild-run, ring buffers, single-source
submodules, etc. all apply here. New vbrecomp-specific memories should
be tagged so future sessions can tell them apart.

---

## 10. No speculative progress

If a step involves:

- indirect jumps or computed dispatch
- relocation or address aliasing
- hardware MMIO interaction (VIP, VSU, IRQ controller, timer)

You MUST produce:

- a manifest documenting what was changed
- a proof artifact (oracle comparison, decoder report, screenshot)

Code without proof is invalid.

---

## 11. First milestone is absolute

Before any Phase 2 work:

- this skeleton must build
- `python -m unittest discover recompiler/tests` must pass
- TCP `ping` on the runtime must return `{"ok":true}`
- the decoder must produce a non-empty opcode-coverage report on
  a homebrew test ROM

No exceptions.

---

## 12. Unknown is acceptable. Guessing is not.

If something is unknown:

→ STOP
→ produce an artifact showing what's unknown (decoder report listing
  unknown opcodes, address ranges with no decoded instructions,
  etc.)

Do NOT guess behaviour.

---

## 13. Broken tooling is never acceptable. Fix it when identified.

If a tool, command, or verification mechanism fails or returns
unexpected results:

→ Fix the tool, immediately, the moment you identify the breakage.
→ Do NOT route around it with indirect evidence.
→ Do NOT infer correctness from two broken implementations agreeing.
→ Do NOT carry the breakage forward as a known limitation.

"The screenshot command returns black" is not a reason to skip visual
verification. It is a reason to fix the screenshot command.

"Both runtime and oracle show the same wrong value" does not make the
value correct. It means both have the same bug — or, more likely, the
comparison harness is broken.

---

## 14. Two independent processes, identical debug harness

vbrecomp runs two processes for cross-checking, NEVER one process
with both backends in it:

- **`vb-runtime.exe`** — recompiled V810 only. SDL window, keyboard
  input, TCP debug server on port **4390**.
- **`vb-beetle.exe`** — Beetle VB (mednafen-vb libretro core) only.
  SDL window, keyboard input, TCP debug server on port **4391**.

Both binaries expose the **same JSON wire protocol** for debug
commands — `read_ram`, `press`, `set_input`, `clear_input`,
`pad_status`, `wtrace_*`, `fntrace_*`, `screenshot`, `ping`, etc.

**Why two processes, not one:** the embedded-oracle approach
shares input across both backends in lockstep, which desyncs
constantly. Two independent processes is the only setup where
each backend can be navigated to its own state and queried on
its own timeline. Cross-process comparison is done by querying
both ports from a tool, NOT by sharing memory.

---

## 15. Phase 5 gate — VIP / VSU implementation before any commercial cart

`STUBS_TO_FIX.md` lists every known stub. Before Phase 5 work
(loading Mario's Tennis), every stub marked "Phase 5+" must be
implemented and oracle-verified:

- VIP columnar rendering pipeline (BG segments, WORLDS, OBJ groups,
  brightness/column tables, XPSTTS/DPSTTS state machines)
- VSU 6-channel waveform synthesis
- Timer + IRQ controller full coverage

Loading the cart with known stubs is how past projects ended up with
thousands of lines of HLE shims.

---

## 16. Self-modifying / install-at-runtime code (monitor; reinstate if needed)

V810 mainstream games are not known to install dispatch code at
runtime the way the PSX BIOS does. If a game we target turns out to
do this (e.g. a game with a software-loaded mini-VM, or a custom
decompressor that JITs into WRAM), the response is to add a
small interpreter that runs only against PCs in pages written-since-
boot — mirroring `psxrecomp` Rule 18. The interpreter is NOT a
fallback for static code we failed to translate.

Until we have a concrete game that needs this, **do not pre-emptively
build it.** Pre-built interpreters become safety blankets and erode
the no-interpreter rule.

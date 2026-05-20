# Recomp Project Principles

These rules are system-agnostic. They apply to every recomp project in
`F:/Projects/`. They were drafted in `recomp-template/PRINCIPLES.md`
and are copied here verbatim with attribution. Per-project rules live
in `CLAUDE.md`, never weakening these fundamentals.

## Ground Truth

- The original ROM/disassembly and a trusted interpreter/oracle are
  the behavioural source of truth.
- Generated C is evidence, not authority. If generated C is wrong,
  fix the recompiler, runtime, analyzer, or game metadata, then
  regenerate.
- Before debugging a symptom, verify which runner, generated source
  tree, executable, ROM, and config are actually used by the build
  you are running.

## Tool Skepticism

- Treat every tool result as untrusted until validated against
  another source or a known-good case.
- Validate first outputs manually: grep results, Ghidra labels,
  generated call targets, TCP commands, frame logs, screenshots,
  and diff tools.
- If observability is missing, extend the structured debug surface
  (TCP/rings/traces/snapshots). Do not build conclusions on ad hoc
  printf spam.

## Hints Are Not Correctness

- CFG/config hints are bootstrap aids, not a first-class model of
  the program.
- A hint may expose a missing edge, function, table, or data shape;
  the proper fix is to improve discovery, decoding, analysis, or
  generation so the next game benefits too.
- Use per-game config only for facts that are genuinely per-game:
  entry points, ROM identity, RAM layout, tables, bank maps, and
  verified metadata.
- Do not paper over a compiler/runtime bug with a per-game hint
  unless a class fix and a mechanical audit are both blocked, and
  document that debt.

## Control Flow Semantics

- Preserve the target CPU's semantics, not the surface shape of
  emitted C.
- Direct calls, tail branches, fallthrough between generated split
  functions, computed dispatch, interrupts, and returns can have
  different stack and status behavior. Model those differences
  explicitly.
- Stack-affecting idioms that skip caller code, synthetic returns,
  delay slots, banked returns, or interrupt frames are class-level
  recompiler/runtime problems. Fix the class and audit all
  instances.
- Host coroutine/fiber stacks are runtime implementation details,
  not guest save-state data.

## Runtime Boundaries

- Bus and memory primitives must be faithful and boring. Do not
  hide a control flow, stack, or lifecycle bug by dropping or
  rewriting arbitrary reads/writes in generic memory accessors.
- If a game needs stack normalization, mode-boundary repair, or
  save-state staging, put that behavior at the explicit
  dispatch/yield/load boundary and document the guest invariant
  being restored.

## Debug Loop

- Find the first divergence, not the final visible bug.
- Classify the failure: discovery/codegen, runtime/timing,
  memory/bus, input, audio/video device emulation, or game
  metadata.
- Trace the writer for state bugs. Trace the executed edge for
  control-flow bugs. Trace device events with cycle/time stamps
  for audio/video bugs.
- After every run, check the project's equivalent of dispatch
  misses or unresolved dynamic calls before deeper debugging.

## Validation

- A fix is done only when the root cause is explained, the class
  of bug is addressed or audited, generated code is refreshed,
  the game builds, and a deterministic smoke or oracle comparison
  exercises the behavior.
- Keep smoke scripts and frame logs deterministic enough that the
  next session can rerun them without reconstructing your manual
  input path.

---

**Attribution:** Copied verbatim from
`F:/Projects/recomp-template/PRINCIPLES.md`.

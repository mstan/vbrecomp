# STUBS_TO_FIX.md

Every known stub in the runtime. The goal is for this file to remain
near-empty: a stub is a fatal abort, not a silent default. If a stub
exists it MUST be listed here with the phase that retires it.

| ID | Subsystem | Description | Retires in |
|----|-----------|-------------|------------|
| —  | (none)    | The skeleton intentionally ships no silent stubs. Every unmapped read/write or unrecognised MMIO register routes through `vb_stub_abort()`. | n/a |

## Rule

A stub is any piece of code that:

- returns a fabricated value (`return 0;`, `return 1;`,
  `*out = some_constant;`)
- silently swallows an unmapped access
- emits a printf/fprintf message in lieu of completing the work
- contains `// TODO`, `// FIXME`, or `// for now` next to control flow

The remedy is **never** "leave it and ship". The remedy is:

1. Implement the missing behaviour, OR
2. Route through `vb_stub_abort("<what>", pc, ...)` and add a row
   to this table with a retiring phase

If you find a stub during a debugging session that isn't listed here:
stop, list it, then proceed.

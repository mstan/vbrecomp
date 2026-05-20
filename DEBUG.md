# DEBUG LOOP (EXECUTION CONTRACT) — vbrecomp

Follow EXACTLY. No deviation. All inspection must go through TCP. See
`TCP.md` for the command surface.

Adapted from `recomp-template/NES/DEBUG.md` with V810 specifics.

---

# RULE 0 — TOOL VALIDATION (FIRST USE)

On the FIRST use of ANY tool:

- Cross-check against another source
- Verify structure AND content
- Confirm assumptions
- Never trust output blindly

If not validated → ALL reasoning is INVALID.

---

# RULE 0a — DISPATCH MISS CHECK (every run, before any debugging)

Before anything else, read `dispatch_misses.log` next to the
executable.

- If non-empty: add all listed functions to the game's TOML under
  `[functions]`, regenerate, rebuild, re-run.
- Repeat until `dispatch_misses.log` is empty.

A game with dispatch misses is FUNDAMENTALLY BROKEN. Do not debug
anything else until resolved.

---

# RULE 0b — ALWAYS-ON RING BUFFER

Probes query rings; probes do NOT arm-then-capture. Per global rule
in `~/.claude/CLAUDE.md`:

- Frame ring (36,000 frames, ~12 min @ 50.27 Hz) is always-on.
- fntrace ring is always-on once `fntrace_arm` is set (it then
  remains armed across sessions until `fntrace_disarm`).
- wtrace ring is always-on once a range is armed.
- Crash ring is always-on (no arming required).
- Freeze heartbeat ring is always-on.

If you find yourself reasoning "the events must have happened
before I attached" — STOP. The ring isn't covering enough. Fix
the ring buffer.

---

# DEBUGGING SURFACES

Pick the one that matches the question:

| Surface | When |
|---------|------|
| **Frame ring buffer** (36k frames) — `get_frame`, `frame_range`, `frame_timeseries`, `history` | "What did frame N look like?" Snapshot comparison across frames. |
| **wtrace_* / fntrace_*** (per-store / per-call rings) | "Which instruction wrote $XXXXXXXX?" or "Which path called function FOO?" Per-event attribution. |
| **Beetle VB oracle on port 4391** | "What does real hardware do here?" Cross-process comparison. |

If the question is "recomp ≠ oracle at byte X at frame Y, why?" —
walk the frame ring buffer backwards first to find the FIRST frame
where X diverged, then switch to wtrace to find the first WRITE that
produced the wrong value, then trace that writer.

---

# THE LOOP

0. Tool validation (first use only)
0a. Dispatch-miss check
0b. Confirm ring buffer covers the window of interest
1. **Sync state** — establish what "the same point in execution" means
   across the two servers you're comparing (not frame number alone — VB
   frame rate is 50.27 Hz, not a round 50 or 60)
2. **Dump full state** — runtime and oracle from the frame ring buffer
   (`get_frame` / `frame_range` / `frame_timeseries`)
3. **Diff bytes** — find bytes that differ
4. **Find FIRST divergence** — walk the ring buffer backwards
5. **Trace the writer** — function + instruction + call path via
   `wtrace_arm` covering the suspect address, `wtrace_dump` for the
   writer history
6. **Classify** — codegen bug / runtime bug / VIP/VSU bug / timing
   bug / config bug
7. **Fix the tool** — recompiler, runtime, or TCP tooling. NEVER edit
   generated output.

If ANY step is skipped → STOP and restart.

---

# HARD RULES

- **No printf debugging.** Ever. If TCP can't see it, extend TCP.
- **No hand-editing generated output.** Fix the generator / runtime /
  game config and regenerate.
- **No guessing.** Every claim must cite measured data, Ghidra
  evidence, or V810 manual reference.
- **State which surface you're on.** "Per the frame ring at f=203"
  or "per `wtrace_dump` at write_idx=1,258,944." Not "probably
  around frame 200."
- **Walk backwards to the first divergence.** Later differences are
  consequences; only the first one has a root cause.

---

# WHEN DOCUMENTING A FINDING

Every debugging response must at minimum state:

1. Target behavior being verified
2. Oracle (Beetle VB TCP 4391 / recompiled runtime TCP 4390 / ROM via Ghidra / V810 manual)
3. Sync point (what makes the two sides comparable)
4. Diff (subsystem, address, expected, actual)
5. First divergence (frame or wtrace seq — not a rough eyeball)
6. Writer (function + PC + call path)
7. Classification
8. Minimal fix proposal
9. Re-test plan

If any section is missing → STOP.

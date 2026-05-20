# TCP.md — Debug Server Protocol (vbrecomp)

The TCP debug server is the ONLY sanctioned debugging interface for
vbrecomp. If a piece of state isn't observable over TCP, extend
`runtime/src/debug_server.c` — do not work around it with printf.

Adapted from `recomp-template/NES/TCP.md` with V810 / VB specifics.

---

# RING BUFFER (CRITICAL)

The always-on surface records a full state snapshot every frame into a
**36,000-entry ring buffer** (~12 minutes at 50.27 Hz, the Virtual
Boy's native frame rate).

Each frame record contains:
- CPU registers (32 GPRs, PC, PSW exploded flags, system registers)
- VIP state (XPSTTS, DPSTTS, INTPND, INTENB, brightness, column tables)
- VSU state (channel enables, waveform pointers, envelopes)
- Pad state (16-bit register, last press/release events)
- Interrupt controller state (pending mask, in-service mask)
- Timer state (TLR, THR, TCR, TIH)
- Last executed function name
- Game-specific data (32 bytes, filled by game hook)

All retroactive inspection commands read from this buffer. If you
cannot answer a question from live state, query the history.

---

# PORTS

| Process              | Port  | Configurable via |
|----------------------|-------|------------------|
| `vb-runtime.exe`     | 4390  | `debug.ini` `runtime.debug_port` or `--port N` |
| `vb-beetle.exe`      | 4391  | `debug.ini` `oracle.debug_port` or `--port N` |

Default `127.0.0.1`. One client at a time.

---

# TRANSPORT

- TCP localhost
- Line-based; one command per line terminated by `\n`
- JSON request preferred: `{"cmd":"read_ram","addr":"0x05000000","len":32,"id":7}`
- Bare request accepted for the simplest commands: `ping\n`
- Single-line JSON response: `{"ok":true,...}` or `{"ok":false,"error":"..."}`
- `id` echoed when supplied
- Max command line: 8192 bytes

---

# COMMAND SURFACE

## Monitoring & Inspection

```
ping                    frame                 quit
get_registers           read_ram              dump_ram             write_ram
vip_state               vsu_state             irq_state            pad_state
psw_state               timer_state           screenshot
# vip_state: runtime returns full state (INT*/D*CTRL/D*STTS/X*/BRT*/
#            FRMCYC/BKCOL/SPT/GPLT/JPLT + column/region/drawing/cycles).
#            Oracle (vb-beetle) returns the writable-register subset
#            exposed by mednafen's VIP_GetRegister; internal state-
#            machine vars require a beetle-vb patch.
opcode_coverage         function_listing      memory_map
```

## Cross-process tooling (Python)

```
tools/_wtrace_summary.py     — paginated drain of wtrace + histograms by
                               4 KB target page and source PC
tools/_wram_diff.py          — byte-level WRAM diff between vb-runtime
                               (4390) and vb-beetle (4391); paginated reads
```

## State Manipulation (debug/test only)

```
psw_set       — overwrite PSW (unpacks into exploded fields)
                {"cmd":"psw_set","value":"0x00000001"}
irq_force     — assert or deassert an IRQ source line
                {"cmd":"irq_force","source":1,"asserted":1}
                Sources: 0=INPUT 1=TIMER 2=EXPANSION 3=COMM 4=VIP
```

## Ring-Buffer Queries

```
history                 get_frame             frame_range          frame_timeseries
read_frame_ram          restore_frame
```

## Execution Control

```
pause                   continue              step                 run_to_frame
set_input               press                 clear_input
```

## Comparison & Verification

```
frame_diff              memory_diff           first_divergence     framebuf_diff
vip_diff                # tools/_vip_diff.py — cross-process VIP register diff
```

## wtrace (per-store ring) — ALWAYS-ON

Every store the cart issues (via the recompiled `cpu->writeN` thunks
into `vb_write{8,16,32}`) is recorded into a 1M-entry circular ring
from process start. There is no arming. Probes query the ring for
the seq window of interest. Source PC is sampled from the active
CPU's pc, which the emitter sets to the current instruction's
address before each store.

```
wtrace_stats        — total, capacity, wrapped, oldest_seq, newest_seq
wtrace_dump         — slice the ring with filters:
                        addr_min/addr_max   (target address range, exclusive max)
                        pc_min/pc_max       (source PC range, exclusive max)
                        from_seq/to_seq     (absolute index window)
                        tail                (last N seqs before filtering)
                        limit               (max entries returned; ≤4096)
                      Each entry: {seq, cycle, pc, addr, value, width, region}
wtrace_reset        — zero the seq counter (rarely needed)
```

## fntrace (per-call ring) — ALWAYS-ON

Every recompiled function body records its entry into a 256K-entry
ring (seq, cycle, pc, lp). Multi-leader functions only record on
the "fresh call" path (after the leader-routing switch's default
branch), so yielded resumes do not flood the ring. Single-leader
functions record every entry; downstream analysis can collapse
repeat-same-pc runs.

```
fntrace_stats       — total, capacity, wrapped, oldest_seq, newest_seq
fntrace_dump        — slice the ring with filters:
                        pc_min/pc_max       (callee range, exclusive max)
                        lp_min/lp_max       (caller return-pc range)
                        from_seq/to_seq
                        tail / limit
                      Each entry: {seq, cycle, pc, lp}
fntrace_reset
```

## Diagnostics

```
dispatch_miss_info      watchdog_status       crash_status         freeze_status
```

## Disassembly (oracle side, used for L1 decoder validation)

```
disasm                  disasm_range
```

---

# DISPATCH MISSES

Dispatch misses are logged to `dispatch_misses.log` next to the
executable. This file is the PRIMARY source — check it after EVERY
runtime run. `dispatch_miss_info` via TCP returns the same data live.

A dispatch miss means `vb_dispatch(addr)` found no generated function.
The game skips that entire subroutine. This is a SILENT GAME-BREAKING
BUG.

Resolution: add entries to the game's TOML under `[functions]`,
regenerate, rebuild.

---

# ADDING A NEW COMMAND

1. Add a `handle_xxx` function in `runtime/src/debug_server.c`.
2. Register it in the `s_commands[]` dispatch table.
3. Mirror it on the oracle side (`runtime/src/beetle_debug_server.c`)
   if it inspects emulator-internal state. Use a matching command
   name so tools can switch ports.
4. Document it in this file under the right section.
5. Rebuild the runtime.
6. **Never** add a side-channel debug log. If TCP can't see it, TCP
   needs to grow until it can.

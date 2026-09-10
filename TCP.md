# Virtual Boy debugger protocol

Listeners bind to localhost, one client per process: runtime port 4390 and
Beetle port 4391, overridden by `--port`. Start with `--paused` for deterministic
control. Build with `VBRECOMP_DEBUG_TOOLS=ON`; per-instruction history also needs
`VBRECOMP_CPUHOOK=ON`. Production builds can omit both.

Send one JSON object per LF-terminated line (CRLF accepted). Fragmented and
coalesced TCP packets are supported. Integer `id` values are echoed, including
errors. Responses are single JSON lines with `ok`. Use forward slashes in paths.
Requests are limited to 8191 bytes; pending replies are bounded to 64MiB and
handle partial sends. Slow readers apply backpressure. An oversized request or
exhausted output queue disconnects that client without stopping the game.

`python tools/debug_client.py --port 4390 capabilities` reports the executable's
implemented commands. The client maintains a connection and accepts any command:

```text
python tools/debug_client.py get_registers
python tools/debug_client.py read_ram addr=0x05000000 len=65536
python tools/debug_client.py step count=1
python tools/debug_client.py breakpoint pc=0xFFFFFFF8
python tools/debug_client.py breakpoint pc=-1
```

| Command | Arguments and behavior |
|---|---|
| `pause`, `continue`, `quit` | Acknowledged control; quit drains pending replies. |
| `run_frames` | `frames=1..10000`; returns absolute `target`. Poll `frame` until target; then CPU stays paused. |
| `get_registers` | PC, all GPRs, packed PSW, exception registers, frame. Runtime also reports cycles. |
| `step` | Runtime only, paused non-halted CPU, `count=1..1000000`; stops after count or on HALT/breakpoint. Poll `execution_stats.stopped`. |
| `breakpoint` | Runtime only, one even `pc`; `-1` clears. Resume skips a breakpoint at the current PC once. |
| `execution_stats` | Runtime only: mode, native entries/instructions, interpreted/fallback counts, first/last fallback PC, stopped state. |
| `set_input`, `clear_input` | `pad` or `mask` uses pressed hardware bits. A=4, Start=4096; a TCP override remains active in a window until `clear_input`; `pad_state` also includes connected bit 2. |
| `read_ram` | `addr`, `len=1..65536`. Hex bytes. Runtime rejects unmapped ranges. Oracle exposes VIP RAM, WRAM, SRAM, ROM; inspect `filled` for unsupported regions. |
| `write_ram` | Paused WRAM/SRAM byte write: `addr`, `val=0..255`. |
| `screenshot` | `path`, `eye=0|1`, native framebuffer by default. Runtime supports presentation/UI capture through existing capture options. |
| `device_state` | Canonical timer, controller, VIP and VSU counters/registers/tables; schema 1 in `tools/device_schema.json`. |
| `cpuhook` | `start`, `max=1..65536`; absolute history window, explicit `head`, `resident_lo`, `begin`, `returned`. |
| `history`, `get_frame` | Runtime: `start`, `count=1..256`; frame records containing PC, PSW and 32 GPRs. |

CPU history retains the last 8,388,608 boundaries (192MiB), including IRQ entry
checks. Each little-endian 24-byte record is `<IIIIQ`: PC, PSW, FNV-1a of r1..r31,
version=2, cumulative cycle. Records precede instruction execution. History wraps;
clients must check eviction instead of silently skipping missing records. Frame
history holds 36,000 records. Separate write, function, VIP phase, audio and WRAM
hash rings expose their own windows. A register hash is diagnostic evidence,
not a byte-level proof of all architectural state.

Shared commands: `audio_pcm`, `capabilities`, `clear_input`, `continue`, `cpuhook`, `device_state`, `frame`, `get_registers`, `memory_map`, `pad_state`, `pause`, `ping`, `quit`, `read_ram`, `run_frames`, `screenshot`, `set_input`, `vip_phase`, `vip_state`, `wram_hash`, `write_ram`.

Additional runtime commands: `audio_shadow_state`, `breakpoint`, `capture_dump`, `execution_stats`, `fntrace_dump`, `fntrace_reset`, `fntrace_stats`, `get_frame`, `history`, `irq_force`, `irq_state`, `overrides_state`, `press`, `psw_set`, `psw_state`, `recolor_reload`, `recolor_state`, `recolor_trace`, `source_dump`, `step`, `timer_state`, `watchdog`, `world_map`, `world_trace`, `wram_anchors`, `wtrace_dump`, `wtrace_reset`, `wtrace_stats`.

The oracle intentionally remains an independent emulator process. Its frontend
supports frame control and inspection; instruction stepping/breakpoints belong
to the recomp runtime. No guest state is copied between the two.

See [PARITY.md](docs/PARITY.md) for the validation workflow.

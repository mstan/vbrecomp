# Virtual Boy development parity

Zero Racers is the integration title for interpreter fallback, deterministic
TCP control, independent comparison, and the shared recomp-ui host. The first
bring-up uses the 1 MiB cartridge with CRC32 `71553796`. ROMs, generated game
code, local saves, and capture artifacts stay outside version control.

## Execution

`--execution hybrid` is the default. An exact guest-PC table selects generated
code. A missing entry executes one V810 instruction through the live CPU and
bus, then tries the native table again. This covers RAM code and static
discovery gaps without inventing function boundaries or changing return
registers. Every decoded native instruction is a legal resume/stop point.

`--execution interpreter` fetches every instruction from the live bus.
`--execution native` is a diagnostic that stops on missing generated code.
`execution_stats` reports the selected mode, native/interpreted instruction
counts, and first/last fallback PCs. Integer execution is implemented separately
in the emitter and interpreter. Floating-point and bit-string helpers are
shared, as are devices and instruction timing; comparing these two modes alone
therefore cannot establish independent hardware accuracy.

Device deadlines, timer accesses, serial controller transfers, interrupt
unmasking, and load/store pairing all use guest time. Save RAM is 64 KiB with
hardware mirroring. Interactive runs save on clean exit; `--save PATH` selects
a file, `--no-save` disables persistence. Headless runs use no save unless an
explicit path is supplied. Invalid-size saves fail without overwriting them.

## Independent reference

`vb-beetle` hosts a separate Beetle VB core in a separate process. It has its
own CPU, memory, devices, event schedule and input latch. No state or bus-read
results are copied from the recomp into the reference. Both receive the same
ROM and externally scheduled inputs.

`tools/prepare-oracle.ps1 -Destination <new directory>` checks out upstream
`1275bd7bddf2166be5a10e45c26c5c2a61370658`, applies the bundled observation
patch, and builds its static library with native Windows MinGW tools. Set
`BEETLE_VB_ROOT` when configuring vbrecomp. Existing oracle trees are preserved.
The patch adds observation only; fixes belong in the recomp or its host.

The reference frontend requests side-by-side, red output and retains both
eyes independently. Its normal window shows the left eye; TCP screenshots can
select `eye:0` or `eye:1`. Default core options must be explicit when their
frontend defaults differ from the core's internal initial values.

## Comparison surface

The line-framed protocol and capability negotiation are documented in
[TCP.md](../TCP.md). `tools/debug_client.py` provides the persistent client and
a command-line interface. `--paused` parks at reset. `run_frames` advances an
exact number of 397,824-cycle display periods and parks again. Native and
interpreter modes also support exact instruction steps and a PC breakpoint.

`tools/cosim.py compare` advances two owned processes through the same route
and stops at the first differing checkpoint. A route is a JSON list of
`{"frames": 300, "pad": 0}` segments; each pad mask applies before that segment.
The default comparison includes:

- PC, all 32 GPRs, PSW, exception PC/status registers and ECR;
- all 64 KiB WRAM, 64 KiB SRAM and 256 KiB mapped VIP memory, including both
  framebuffer pairs, character RAM, world/affine tables and OAM;
- VIP registers and canonical internal timer, controller, VIP and VSU fields;
- both rendered eyes and the selected presentation path;
- stereo PCM at matching absolute sample indices, without time-shifting waves.

`device_state` schema 1 enumerates registers, counters, latches, palettes,
wave/modulation tables, envelopes, noise LFSR and last channel outputs in
[device_schema.json](../tools/device_schema.json). Fields use explicit unsigned
32-bit values; signed counters use two's-complement representation. Pointer
values, struct padding, host output queues, and derived renderer caches are
excluded. This is a declared comparison surface, not a claim that untested
hardware behavior or every possible game route is correct.

Every RAM/image/audio hash is checked against the actual captured bytes even
when hashes agree. Reports name differing CPU/device fields or the first
different byte, include ROM/executable hashes, and preserve the mismatching
byte planes. An exception, short read, incompatible schema, eviction, or
nonzero process exit fails the run.

`cosim.py trace` drills into an absolute instruction/IRQ boundary window.
Version 2 continuously retains eight million records of PC, full PSW,
FNV-1a of r1 through r31, and the guest cycle. Both sequence and cycle are
compared; no records are skipped to manufacture alignment. `--start` selects
a resident absolute index. A PC-only selection is diagnostic, not CPU parity.
This hash trace localizes execution changes; exact checkpoint register values
remain the stronger state comparison.

## Validation commands

Pass `--runtime <game.exe> --oracle <vb-beetle.exe> --rom <cartridge>` to each
tool. Use distinct port pairs for concurrent runs and a new output directory
for each evidence set. Keep binaries fixed for the duration of a run.

```text
python tools/cosim.py compare <paths> --route <race-route.json> --out <captures>
python tools/cosim.py gates <paths> --route <race-route.json> --out <gates>
python tools/cosim.py trace <paths> --frames 10 --instructions 2900000 --out <trace>
python tools/validate_debugger.py <paths> --out <tcp-checks>
python -m unittest discover -s recompiler/tests
ctest --test-dir <build> --output-on-failure
```

`gates` checks recomp/recomp, interpreter/interpreter, recomp/interpreter and,
when an oracle is supplied, oracle/oracle determinism. It deliberately flips
one WRAM byte and requires the coordinator to identify that exact byte at the
first checkpoint. The negative run's `passed:false` is expected; the enclosing
gate must report `injected_fault_detected:true`.

The socket conformance test exercises fragmented and coalesced JSON, CRLF,
request IDs, pause stability, exact frame/step/breakpoint controls, input masks,
full RAM reads/writes, backpressure, continuous trace rollover, invalid reads,
reconnection, clean quit, and save round-trips/rejection.

The method follows the independent-state, first-divergence and injected-fault
requirements in the sibling PSX `docs/internal/COSIM_ORACLE.md` and SNES
`SNES_COSIM.md`. Virtual Boy fallback remains available in production as
explicitly requested for this project. `VBRECOMP_DEBUG_TOOLS=OFF` removes the
TCP/diagnostic frontend; `VBRECOMP_CPUHOOK=OFF` removes CPU trace recording.

## Integration evidence

The Zero Racers host stores repeatable routes under `tests/` and keeps detailed
captures outside version control. The final 39-checkpoint driving route passes
through frame 4000 across all ten comparison planes, with 273 byte audits.
It executes 888,495,917 native instructions and 23,922 fallback instructions.
Hybrid and interpreter determinism, hybrid/interpreter equivalence, independent
oracle determinism and exact one-byte fault detection all pass on that route.

A separate foreground menu/start comparison passes 41 checkpoints through
frame 2400 with neutral controls after race selection: both eyes are identical
to Beetle, and 16 menu/HUD captures match the window's presentation texture.
The earlier preview also matches those left-eye captures. The user inspected
the live side-by-side comparison and reported that they appear identical.

Mario's Tennis regression passes 15 checkpoints through frame 1000 across all
ten planes. The generic interpreter host passes its ten-frame comparison.
Zero Racers CTest passes 4/4; Mario's Tennis CTest passes 5/5; the Python
recompiler suite runs 79 tests: 74 pass and 5 optional oracle tests are skipped. These
results cover the declared routes and comparison fields, not every game mode.

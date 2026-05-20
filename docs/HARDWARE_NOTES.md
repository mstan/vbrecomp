# HARDWARE_NOTES.md — Virtual Boy / NEC V810

Authoritative-ish notes used by the recompiler and runtime. Cross-
references against the NEC V810 Architecture Manual, Virtual Boy
Programmer's Manual, and the Planet Virtual Boy Sacred Tech Scroll
should be confirmed in person; do not treat this file as a
substitute for the manuals.

## CPU — NEC V810 (NVC variant)

- 32-bit RISC, 20 MHz.
- 32 general-purpose registers (`r0`..`r31`), all 32-bit. `r0`
  hardwired to zero. By convention `r29` = stack pointer (sp);
  `r31` = link register (lp), written by `JAL`. `r1`..`r5` are
  caller-saved arguments / scratch in the common ABI.
- Program Counter (PC), 32 bits.
- 32 system registers accessed via `LDSR` / `STSR`:
  | # | Name  | Purpose                                              |
  |---|-------|------------------------------------------------------|
  | 0 | EIPC  | Exception/Interrupt PC                               |
  | 1 | EIPSW | PSW saved on exception/interrupt                     |
  | 2 | FEPC  | Fatal exception PC (NMI / duplicate exception)       |
  | 3 | FEPSW | Saved PSW for fatal exception                        |
  | 4 | ECR   | Exception cause register (low 16: EICC, high: FECC)  |
  | 5 | PSW   | Program status word                                  |
  | 6 | PIR   | Processor ID register (read-only)                    |
  | 7 | TKCW  | Task control word (float trap enables)               |
  | 24| CHCW  | Cache control word                                   |
  | 25| ADTRE | Address trap register for exec                       |

  Other indices are reserved/undefined; LDSR to a reserved index is
  undefined behaviour — we **fatal abort** on attempted use rather
  than silently dropping.

- **PSW flag bits:**
  | bit  | name | meaning                              |
  |------|------|--------------------------------------|
  | 0    | Z    | Zero                                 |
  | 1    | S    | Sign                                 |
  | 2    | OV   | Signed overflow                      |
  | 3    | CY   | Carry / unsigned overflow            |
  | 4    | FPR  | FP precision (reserved on V810 NVC)  |
  | 5    | FUD  | FP underflow                         |
  | 6    | FOV  | FP overflow                          |
  | 7    | FZD  | FP zero divide                       |
  | 8    | FIV  | FP invalid                           |
  | 9    | FRO  | FP reserved op                       |
  | 12   | ID   | Interrupt disable                    |
  | 13   | AE   | Address-trap enable                  |
  | 14   | EP   | In exception                         |
  | 15   | NP   | In NMI                               |
  |16-19 | I    | Interrupt level / enable level (0..15) |

- **Six instruction formats:** I (1-reg), II (5-bit imm + reg), III
  (cond branch, 9-bit disp), IV (jump, 26-bit disp), V (16-bit imm),
  VI (load/store), VII (FP and bitstring extensions). Standard 16-bit
  opcode; format V/VI/VII have a 32-bit width via a trailing
  16-bit halfword.
- **No branch delay slot.** Conditional branches and `JR`/`JAL`/`JMP`
  are atomic with respect to the following instruction.
- **Bitstring instructions** (formats VII): SCH0BSU, SCH1BSU,
  SCH0BSD, SCH1BSD, ORBSU, ANDBSU, XORBSU, MOVBSU, ORNBSU, ANDNBSU,
  XORNBSU, NOTBSU. They consume `r26..r30` implicitly as a bit-
  pointer pair (src/dst addr + bit offset + length). Long-running
  and interrupt-restartable.
- **Floating point** (format VII): ADDF.S, SUBF.S, MULF.S, DIVF.S,
  CMPF.S, CVT.WS, CVT.SW, TRNC.SW. IEEE-754 single. Exceptions
  delivered via PSW float flags + FRO.
- **Special caches:** 1 KB on-chip instruction cache configured via
  CHCW. We model writes-to-CHCW as logged events; cache is
  transparent to recompiled code (no I-cache effects).

## Memory map (27-bit decoded; high bits mirror)

```
0x00000000 – 0x00FFFFFF   VIP registers + VRAM (mirrored)
0x01000000 – 0x01FFFFFF   VSU registers + WRAM (mirrored)
0x02000000 – 0x02FFFFFF   Misc hardware (timer, pad, link, wait)
0x03000000 – 0x03FFFFFF   reserved (unmapped → fatal)
0x04000000 – 0x04FFFFFF   Cartridge expansion
0x05000000 – 0x05FFFFFF   WRAM (64 KB, mirrored)
0x06000000 – 0x06FFFFFF   Cartridge RAM (save SRAM when present)
0x07000000 – 0x07FFFFFF   Cartridge ROM (mirrored)
0x08000000 – 0xFFFFFFFF   alias of 0x00000000–0x07FFFFFF (27-bit mirror)
```

`0xFFFFFFF0` (reset vector) folds to `0x07FFFFF0` in cart ROM. Cart
top 16 bytes hold the reset-vector table (reset, NMI, exception,
INT0..INTn).

## VIP — Virtual Image Processor (display)

- Renders 384 × 224 pixels per eye at 50.27 Hz native, stereo via
  fast-switching mirrors.
- Pixel depth: 2 bits per pixel + 4-level brightness per pixel.
- Architecture: column-major rendering. 8 × 32-column blocks render
  each frame. Status registers XPSTTS (rendering) and DPSTTS
  (display) gate column progression; software polls these.
- Memory: framebuffer (left/right), character RAM (8 KB chars), BG
  segments (2-KB tilemap per segment, up to 14 segments), WORLDS
  table (32 entries describing windowed BG layers with parallax /
  affine-ish transformations), OBJ groups (sprites).
- Interrupts: SBHIT, LFBEND, RFBEND, GAMESTART, XPEND, TIMEERR,
  SCANERR. INTPND/INTENB/INTCLR registers.
- Brightness control: BRTA/BRTB/BRTC, REST table, columnar timing.

## VSU — Virtual Sound Unit (audio)

- 6 channels:
  - Channel 1..4: 32-byte 6-bit-PCM wave RAM playback with sweep /
    envelope.
  - Channel 5: wave RAM with modulation (FM-ish).
  - Channel 6: 32-bit LFSR noise.
- Each channel has interval timer, envelope (level + sweep), stereo
  pan (LRV register), waveform pointer.
- Wave RAM at 5 × 128 bytes (WRAM-mapped via mirror).

## Pad / input

- 16-bit pad register at `0x02000028` (read). Bits:
  | bit | name  |
  |-----|-------|
  | 0   | LDOWN |
  | 1   | LLEFT |
  | 2   | LUP   |
  | 3   | LRIGHT|
  | 4   | RDOWN |
  | 5   | RLEFT |
  | 6   | RUP   |
  | 7   | RRIGHT|
  | 8   | A     |
  | 9   | B     |
  | 10  | START |
  | 11  | SELECT|
  | 12  | LT    |
  | 13  | RT    |
  | 14  | low battery |
  | 15  | always 0 (signature bit)|

  Active LOW with a pull-up — common convention is to invert
  on read so a pressed key reports as 1.

## Timer

- 20-bit programmable down-counter. Register page in MISC space:
  | Addr (16 MB mirrored) | R/W | Field                                       |
  |-----------------------|-----|---------------------------------------------|
  | `0x02000018`          | R   | TimerCounter low byte                       |
  | `0x02000018`          | W   | TimerReloadValue low byte (sets ReloadPending) |
  | `0x0200001C`          | R   | TimerCounter high byte                      |
  | `0x0200001C`          | W   | TimerReloadValue high byte (sets ReloadPending) |
  | `0x02000020`          | R   | TimerControl `| 0xE0 | TC_ZSTATCLR | (TimerStatus ? TC_ZSTAT : 0)` |
  | `0x02000020`          | W   | TimerControl + ZSTATCLR-on-write semantics  |
- Strides are 4 bytes. The misc-page bus rejects writes with
  `A & 3 != 0` (Beetle `TIMER_Write`); odd-byte reads return 0.
- TCR bits: `TENABLE=0x01`, `ZSTAT=0x02` (read-only),
  `ZSTATCLR=0x04` (write-1-to-clear), `TIMZINT=0x08` (IRQ enable),
  `TCLKSEL=0x10` (0 = 100µs / 2000 cycles, 1 = 20µs / 400 cycles).
- IRQ source: `VBIRQ_SOURCE_TIMER (= 1)`, asserted when
  `TimerStatusShadow && (TimerControl & TIMZINT)`.

## Interrupt controller

- 5 active sources (Beetle `VBIRQ_SOURCE_*`):
  | Bit | Source     | Level | Vector       | Trigger                                |
  |-----|------------|-------|--------------|----------------------------------------|
  | 0   | INPUT      | 0     | `0xFFFFFE00` | Pad IRQ on signature-bit transition    |
  | 1   | TIMER      | 1     | `0xFFFFFE10` | 20-bit timer underflow + TIMZINT       |
  | 2   | EXPANSION  | 2     | `0xFFFFFE20` | Cartridge expansion request            |
  | 3   | COMM       | 3     | `0xFFFFFE30` | Link-port communications               |
  | 4   | VIP        | 4     | `0xFFFFFE40` | INTPND & INTENB                        |
- Higher level wins; PSW.I gates levels strictly below itself.
- Acceptance gate (Beetle `RecalcIPendingCache`):
  - `PSW & (NP | EP | ID) == 0`, AND
  - `ilevel >= (PSW & PSW_IA) >> 16`
- On acceptance (Beetle `op_INT_HANDLER` at `v810_oploop.inc:977`):
  ```
  EIPC  = PC
  EIPSW = PSW
  PC    = 0xFFFFFE00 | (level << 4)
  ECR   = 0xFE00 | (level << 4)
  PSW  |= EP | ID
  PSW  &= ~AE
  PSW.I = min(level + 1, 15)
  Halted = 0
  ```
- TRAP imm5 (Beetle `v810_oploop.inc:929`):
  ```
  Exception(TRAP_HANDLER_BASE + (imm5 & 0x10), ECODE_TRAP_BASE + (imm5 & 0x1F))
  # TRAP_HANDLER_BASE = 0xFFFFFFA0
  # ECODE_TRAP_BASE   = 0xFFA0
  ```
- Exception() entry rules (Beetle `v810_cpu.cpp:1111`):
  - PSW.NP set → `Halted = HALT_FATAL_EXCEPTION`, no entry.
  - PSW.EP set → double-fault: `FEPC=PC`, `FEPSW=PSW`,
    `ECR.hi = eCode`, `PSW |= NP|ID`, `PSW &= ~AE`, `PC = 0xFFFFFFD0`.
  - Else: regular: `EIPC=PC`, `EIPSW=PSW`, `ECR.lo = eCode`,
    `PSW |= EP|ID`, `PSW &= ~AE`, `PC = handler`.
- RETI (Beetle `v810_oploop.inc:793`):
  - if PSW.NP: `PC = FEPC & ~1`, `PSW = FEPSW`
  - else:      `PC = EIPC & ~1`, `PSW = EIPSW`
- Acknowledge: writing `INTCLR` (VIP), `INPUT.S1HW` (pad), `TCR.ZSTATCLR`
  (timer) clears the source. The IRQ controller re-asserts immediately
  if the underlying source-active condition is still present.

## Cycle counts / timing

- CPU at 20 MHz, most ALU = 1 cycle, multiply = 13, divide = 38,
  shift = 1+N variable, load = 4 (cache miss), branch taken = 3.
- Per-frame budget: 20 000 000 / 50.27 ≈ **397 850 cycles per frame**.
- VIP synchronisation: software typically waits for `DPSTTS.FCLK`
  toggle (per-frame) or `XPSTTS.XPBSY` to know when columns finish.

---

**Sources to consult when populating implementation details:**

- NEC V810 Architecture Manual (Planet Virtual Boy archive)
- Virtual Boy Programmer's Manual
- Planet Virtual Boy Sacred Tech Scroll
- Mednafen Beetle VB source (for behavioural ground truth)
- Rustual Boy (secondary reference)
- Ghidra V810/V830 SLEIGH module (for static-disassembly comparison)

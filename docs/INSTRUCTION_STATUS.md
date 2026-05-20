# INSTRUCTION_STATUS.md — V810 opcode coverage

**Single source of truth** for V810 implementation status.

This table is regenerated mechanically by
`python -m recompiler.cli.vbrecomp_coverage --regen-status`.
Do not hand-edit — the regenerator overwrites it.

Columns:

- **opcode** — V810 mnemonic
- **format** — I / II / III / IV / V / VI / VII
- **decoded** — `yes` / `Invalid` — does `decoder.py` recognise the opcode bits and extract operand fields?
- **lifted** — `yes` / `no` — does `lifter.py` produce IR? (P3+)
- **emitted** — `yes` / `no` — does `emitter.py` produce C? (P3+)
- **oracle** — `yes` / `N/A` — does our mnemonic agree with Beetle VB's static op table?

`Invalid` means NEC documents the slot as reserved AND Beetle traps to `op_INVALID`; the decoder reports `is_unknown=True`. `N/A` in the oracle column appears for slots Beetle invalid-traps (no mnemonic to compare) and for sub-op dispatchers (where the oracle check is per sub-op).

## Format I — register-register (2-byte; 6-bit opcode + reg2 + reg1)

| opcode | format | decoded | lifted | emitted | oracle |
|--------|--------|---------|--------|---------|--------|
| MOV | I | yes | no | no | yes |
| ADD | I | yes | no | no | yes |
| SUB | I | yes | no | no | yes |
| CMP | I | yes | no | no | yes |
| SHL | I | yes | no | no | yes |
| SHR | I | yes | no | no | yes |
| JMP | I | yes | no | no | yes |
| SAR | I | yes | no | no | yes |
| MUL | I | yes | no | no | yes |
| DIV | I | yes | no | no | yes |
| MULU | I | yes | no | no | yes |
| DIVU | I | yes | no | no | yes |
| OR | I | yes | no | no | yes |
| AND | I | yes | no | no | yes |
| XOR | I | yes | no | no | yes |
| NOT | I | yes | no | no | yes |

## Format II — register + 5-bit immediate (2-byte)

| opcode | format | decoded | lifted | emitted | oracle | notes |
|--------|--------|---------|--------|---------|--------|-------|
| MOV | II | yes | no | no | yes |  |
| ADD | II | yes | no | no | yes |  |
| SETF | II | yes | no | no | yes |  |
| CMP | II | yes | no | no | yes |  |
| SHL | II | yes | no | no | yes |  |
| SHR | II | yes | no | no | yes |  |
| CLI | II | yes | no | no | yes | Beetle calls this `op_EI` — same insn |
| SAR | II | yes | no | no | yes |  |
| TRAP | II | yes | no | no | yes |  |
| RETI | II | yes | no | no | yes |  |
| HALT | II | yes | no | no | yes |  |
| `<reserved 0x1B>` | II | Invalid | no | no | N/A | NEC-reserved primary opcode |
| LDSR | II | yes | no | no | yes |  |
| STSR | II | yes | no | no | yes |  |
| SEI | II | yes | no | no | yes | Beetle calls this `op_DI` — same insn |
| BSU | II | yes | no | no | N/A | BSU dispatcher — see Format VII / BSU below |

## Format III — conditional branch (Bcond, 9-bit disp)

| opcode | format | decoded | lifted | emitted | oracle |
|--------|--------|---------|--------|---------|--------|
| BV | III | yes | no | no | yes |
| BL | III | yes | no | no | yes |
| BE | III | yes | no | no | yes |
| BNH | III | yes | no | no | yes |
| BN | III | yes | no | no | yes |
| BR | III | yes | no | no | yes |
| BLT | III | yes | no | no | yes |
| BLE | III | yes | no | no | yes |
| BNV | III | yes | no | no | yes |
| BNL | III | yes | no | no | yes |
| BNE | III | yes | no | no | yes |
| BH | III | yes | no | no | yes |
| BP | III | yes | no | no | yes |
| NOP | III | yes | no | no | yes |
| BGE | III | yes | no | no | yes |
| BGT | III | yes | no | no | yes |

## Format IV — long jump (4-byte; 26-bit disp)

| opcode | format | decoded | lifted | emitted | oracle |
|--------|--------|---------|--------|---------|--------|
| JR | IV | yes | no | no | yes |
| JAL | IV | yes | no | no | yes |

## Format V — register + 16-bit immediate (4-byte)

| opcode | format | decoded | lifted | emitted | oracle |
|--------|--------|---------|--------|---------|--------|
| MOVEA | V | yes | no | no | yes |
| ADDI | V | yes | no | no | yes |
| ORI | V | yes | no | no | yes |
| ANDI | V | yes | no | no | yes |
| XORI | V | yes | no | no | yes |
| MOVHI | V | yes | no | no | yes |

## Format VI — load / store / IN / OUT / CAXI (4-byte; 16-bit disp)

| opcode | format | decoded | lifted | emitted | oracle | notes |
|--------|--------|---------|--------|---------|--------|-------|
| LD.B | VI | yes | no | no | yes |  |
| LD.H | VI | yes | no | no | yes |  |
| `<reserved 0x32>` | VI | Invalid | no | no | N/A | NEC-reserved primary opcode |
| LD.W | VI | yes | no | no | yes |  |
| ST.B | VI | yes | no | no | yes |  |
| ST.H | VI | yes | no | no | yes |  |
| `<reserved 0x36>` | VI | Invalid | no | no | N/A | NEC-reserved primary opcode |
| ST.W | VI | yes | no | no | yes |  |
| IN.B | VI | yes | no | no | yes |  |
| IN.H | VI | yes | no | no | yes |  |
| CAXI | VI | yes | no | no | yes | Sacred Tech Scroll: Format VI (not VII) |
| IN.W | VI | yes | no | no | yes |  |
| OUT.B | VI | yes | no | no | yes |  |
| OUT.H | VI | yes | no | no | yes |  |
| OUT.W | VI | yes | no | no | yes |  |

## Format VII — FPP floating-point + extensions (4-byte; sub-op in 2nd halfword bits 15:10)

| opcode | format | decoded | lifted | emitted | oracle | notes |
|--------|--------|---------|--------|---------|--------|-------|
| CMPF.S | VII | yes | no | no | yes | FPP sub-op 0x00 |
| CVT.WS | VII | yes | no | no | yes | FPP sub-op 0x02 |
| CVT.SW | VII | yes | no | no | yes | FPP sub-op 0x03 |
| ADDF.S | VII | yes | no | no | yes | FPP sub-op 0x04 |
| SUBF.S | VII | yes | no | no | yes | FPP sub-op 0x05 |
| MULF.S | VII | yes | no | no | yes | FPP sub-op 0x06 |
| DIVF.S | VII | yes | no | no | yes | FPP sub-op 0x07 |
| XB | VII | yes | no | no | yes | FPP sub-op 0x08 |
| XH | VII | yes | no | no | yes | FPP sub-op 0x09 |
| REV | VII | yes | no | no | yes | FPP sub-op 0x0A |
| TRNC.SW | VII | yes | no | no | yes | FPP sub-op 0x0B |
| MPYHW | VII | yes | no | no | yes | FPP sub-op 0x0C |

## Format VII — BSU bitstring (2-byte; sub-op in reg2 field bits 9:5)

| opcode | format | decoded | lifted | emitted | oracle | notes |
|--------|--------|---------|--------|---------|--------|-------|
| SCH0BSU | VII | yes | no | no | yes | BSU sub-op 0x00 |
| SCH0BSD | VII | yes | no | no | yes | BSU sub-op 0x01 |
| SCH1BSU | VII | yes | no | no | yes | BSU sub-op 0x02 |
| SCH1BSD | VII | yes | no | no | yes | BSU sub-op 0x03 |
| ORBSU | VII | yes | no | no | yes | BSU sub-op 0x08 |
| ANDBSU | VII | yes | no | no | yes | BSU sub-op 0x09 |
| XORBSU | VII | yes | no | no | yes | BSU sub-op 0x0A |
| MOVBSU | VII | yes | no | no | yes | BSU sub-op 0x0B |
| ORNBSU | VII | yes | no | no | yes | BSU sub-op 0x0C |
| ANDNBSU | VII | yes | no | no | yes | BSU sub-op 0x0D |
| XORNBSU | VII | yes | no | no | yes | BSU sub-op 0x0E |
| NOTBSU | VII | yes | no | no | yes | BSU sub-op 0x0F |

---

## Oracle source

The `oracle` column is checked at regen time against Beetle VB's static dispatch table at
`beetle-vb/mednafen/hw_cpu/v810/v810_op_table_msvc.inc`.

Full per-instruction parity (mnemonic at every CFG-aware reachable PC on a representative cart) is verified by `recompiler/tests/test_decoder_oracle.py`.

"""isa_semantics_check.py — compile-and-run check of the emitter's MUL/DIV
semantics against the oracle (Mednafen v810_oploop.inc).

The Axis-1 latent-bug gate. For each crafted operand case, this takes the
EXACT C body the emitter produces (`_emit_format_i`), compiles + runs it with
those inputs, and compares dest / r30 / PSW flags against the golden values
the oracle would produce. Golden is derived from the oracle's own source
(beetle-vb/mednafen/hw_cpu/v810/v810_oploop.inc, the code compiled into
vb-beetle) — cited per case below.

Targets the four latent bugs the cpuhook proved are never triggered by
Mario's Tennis: MUL/MULU Z-flag (low-32 vs full-64), MUL/MULU/DIV/DIVU
r30-vs-dest write order when dest==r30, and DIV/DIVU ÷0 (trap vs abort).

    python tools/isa_semantics_check.py
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from recompiler.v810.decoder import decode_at
from recompiler.v810.emitter import _emit_format_i

# Format-I opcodes
MUL, DIV, MULU, DIVU = 0x08, 0x09, 0x0A, 0x0B


def enc_fmt_i(op, reg2, reg1):
    hw = (op << 10) | (reg2 << 5) | reg1
    return bytes([hw & 0xFF, (hw >> 8) & 0xFF])


def body_for(op, reg2, reg1):
    ins = decode_at(enc_fmt_i(op, reg2, reg1), 0, 0x07000000)
    return _emit_format_i(ins)


# Each case: name, op, reg1(src), reg2(dest), gpr-inputs, expected outputs.
# expected: dest value (read from `read_reg`), r30, z, s, ov, exc(0/1).
# Golden cited from v810_oploop.inc.
CASES = [
    # MULU 0x10000*0x10000 = 0x1_0000_0000 -> low=0, high=1.
    # SetSZ(low)=> Z=1 (BUG was full-64 => Z=0). OV(MULU)=high!=0 =>1. (l823-831)
    dict(name="MULU low=0,high=1 (Z from low)", op=MULU, r1=4, r2=5,
         inp={4: 0x00010000, 5: 0x00010000}, read=5,
         dest=0, r30=1, z=1, s=0, ov=1),
    # MUL same operands: low=0,high=1; Z=1; OV = product!=sext(low) =>1. (l811-819)
    dict(name="MUL low=0,high=1 (Z from low)", op=MUL, r1=4, r2=5,
         inp={4: 0x00010000, 5: 0x00010000}, read=5,
         dest=0, r30=1, z=1, s=0, ov=1),
    # MULU dest==r30: r30 written first (high), then dest (low) wins => r30=low=0.
    dict(name="MULU dest==r30 (dest wins)", op=MULU, r1=4, r2=30,
         inp={4: 0x00010000, 30: 0x00010000}, read=30,
         dest=0, r30=0, z=1, s=0, ov=1),
    # MUL dest==r30 likewise => r30=low=0.
    dict(name="MUL dest==r30 (dest wins)", op=MUL, r1=4, r2=30,
         inp={4: 0x00010000, 30: 0x00010000}, read=30,
         dest=0, r30=0, z=1, s=0, ov=1),
    # DIVU 100/7 = 14 rem 2.
    dict(name="DIVU 100/7", op=DIVU, r1=4, r2=5,
         inp={4: 7, 5: 100}, read=5, dest=14, r30=2, z=0, s=0, ov=0),
    # DIVU dest==r30: r30=remainder first, then dest=quotient wins => r30=14. (l845-850)
    dict(name="DIVU dest==r30 (dest wins)", op=DIVU, r1=4, r2=30,
         inp={4: 7, 30: 100}, read=30, dest=14, r30=14, z=0, s=0, ov=0),
    # DIV signed 100/-7 = -14 rem 2; S=1 (quotient<0). (l858-883)
    dict(name="DIV 100/-7", op=DIV, r1=4, r2=5,
         inp={4: 0xFFFFFFF9, 5: 100}, read=5,
         dest=0xFFFFFFF2, r30=2, z=0, s=1, ov=0),
    # DIV INT_MIN/-1 special case: q=0x80000000, r=0, OV=1. (l868-874)
    dict(name="DIV INT_MIN/-1 (OV)", op=DIV, r1=4, r2=5,
         inp={4: 0xFFFFFFFF, 5: 0x80000000}, read=5,
         dest=0x80000000, r30=0, z=0, s=1, ov=1),
    # DIVU ÷0 -> zero-division EXCEPTION (handler 0xFFFFFF80, ecode 0xFF80),
    # NOT abort. (l835-842)
    dict(name="DIVU /0 -> exception", op=DIVU, r1=4, r2=5,
         inp={4: 0, 5: 10}, read=5, exc=1),
    # DIV ÷0 -> exception. (l858-865)
    dict(name="DIV /0 -> exception", op=DIV, r1=4, r2=5,
         inp={4: 0, 5: 10}, read=5, exc=1),
]


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <limits.h>

typedef struct {
    uint32_t gpr[32];
    int psw_z, psw_s, psw_cy, psw_ov;
    uint32_t pc;
    uint64_t cycles;
    int exc_called; uint32_t exc_handler; unsigned exc_ecode;
} CPUState;

#define VB_ZERO_DIV_HANDLER 0xFFFFFF80u
#define VB_ECODE_ZERO_DIV   0xFF80u

static void vb_exception(CPUState* cpu, uint32_t handler, uint16_t ecode) {
    cpu->exc_called = 1; cpu->exc_handler = handler; cpu->exc_ecode = ecode;
    cpu->pc = handler;
}

static void run(CPUState* cpu) %BODY%

int main(void) {
    CPUState c; for (int i=0;i<32;i++) c.gpr[i]=0;
    c.psw_z=c.psw_s=c.psw_cy=c.psw_ov=0; c.pc=0; c.cycles=0;
    c.exc_called=0; c.exc_handler=0; c.exc_ecode=0;
%SETUP%
    run(&c);
    printf("%u %u %d %d %d %d %u %u\n",
           c.gpr[%READ%], c.gpr[30], c.psw_z, c.psw_s, c.psw_ov,
           c.exc_called, c.exc_handler, c.exc_ecode);
    return 0;
}
"""


def find_gcc():
    for cand in ("gcc", r"C:\msys64\mingw64\bin\gcc.exe"):
        try:
            subprocess.run([cand, "--version"], capture_output=True, check=True)
            return cand
        except Exception:
            continue
    return None


def main():
    gcc = find_gcc()
    if not gcc:
        print("error: gcc not found (need mingw64 on PATH)", file=sys.stderr)
        return 2

    fails = 0
    with tempfile.TemporaryDirectory() as td:
        for i, c in enumerate(CASES):
            body = body_for(c["op"], c["r2"], c["r1"])
            setup = "".join(f"    c.gpr[{r}]=0x{v:08X}u;\n"
                            for r, v in c["inp"].items())
            src = (HARNESS.replace("%BODY%", body)
                          .replace("%SETUP%", setup)
                          .replace("%READ%", str(c["read"])))
            cf = Path(td) / f"case{i}.c"
            ef = Path(td) / f"case{i}.exe"
            cf.write_text(src)
            r = subprocess.run([gcc, "-O2", "-std=c11", "-o", str(ef), str(cf)],
                               capture_output=True, text=True)
            if r.returncode != 0:
                print(f"FAIL  {c['name']}: compile error\n{r.stderr}")
                fails += 1
                continue
            out = subprocess.run([str(ef)], capture_output=True, text=True)
            vals = out.stdout.split()
            dest, r30, z, s, ov, exc, hdlr, ecode = (int(x) for x in vals)

            ok = True
            detail = []
            if c.get("exc"):
                if not exc:
                    ok = False; detail.append("expected exception, none raised")
                elif hdlr != 0xFFFFFF80 or ecode != 0xFF80:
                    ok = False
                    detail.append(f"handler=0x{hdlr:08X} ecode=0x{ecode:04X}")
            else:
                if exc:
                    ok = False; detail.append("unexpected exception")
                for k, got in (("dest", dest), ("r30", r30), ("z", z),
                               ("s", s), ("ov", ov)):
                    if c[k] != got:
                        ok = False
                        detail.append(f"{k}: got {got} want {c[k]}")
            if ok:
                print(f"PASS  {c['name']}")
            else:
                print(f"FAIL  {c['name']}: " + "; ".join(detail))
                fails += 1

    print("-" * 60)
    print(f"{len(CASES) - fails}/{len(CASES)} passed")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

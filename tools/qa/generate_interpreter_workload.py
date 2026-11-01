"""Create an original distributable V810 workload; never run it implicitly."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--runtime', type=Path, required=True)
p.add_argument('--frames', type=int, default=120)
p.add_argument('--execution', choices=['default', 'hybrid', 'interpreter'], default='default',
               help='Default omits the switch and exercises the generic host hybrid fallback')
a = p.parse_args()
assert 1 <= a.frames <= 1000000
out = a.out.resolve()
out.mkdir(parents=True, exist_ok=False)
rom = bytearray(65536)
pc = 0


def emit(op, r1=0, r2=0, imm=None):
    global pc
    struct.pack_into('<H', rom, pc, (op << 10) | (r2 << 5) | r1)
    pc += 2
    if imm is not None:
        struct.pack_into('<H', rom, pc, imm & 65535)
        pc += 2


emit(0x10, 1, 1)                    # MOV 1,r1
emit(0x2f, 0, 2, 0x0500)            # MOVHI 0x0500,r0,r2
emit(0x28, 2, 2, 0x2000)            # MOVEA 0x2000,r2,r2 (WRAM)
start = pc
emit(0x01, 1, 10)                   # ADD r1,r10
emit(0x0e, 10, 11)                  # XOR r10,r11
emit(0x14, 1, 11)                   # SHL 1,r11
emit(0x37, 2, 11, 0)                # ST.W r11,0[r2]
emit(0x33, 2, 12, 0)                # LD.W 0[r2],r12
emit(0x0c, 12, 10)                  # OR r12,r10
displacement = (start - pc) & 0x1ff
struct.pack_into('<H', rom, pc, 0x8000 | (5 << 9) | displacement)  # BR start
displacement = (-0xfff0) & 0x3ffffff
struct.pack_into('<HH', rom, 0xfff0, (0x2a << 10) | (displacement >> 16), displacement & 65535)
rom_path = out/'shared-interpreter-workload.vb'
rom_path.write_bytes(rom)
argv = [str(a.runtime.resolve()), '--rom', str(rom_path)]
if a.execution != 'default':
    argv += ['--execution', a.execution]
argv += ['--benchmark', str(a.frames), '--no-save', '--no-launcher',
        '--config', str(out/'config.json'), '--mods-dir', str(out/'mods')]
job = dict(argv=argv, env=dict(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy'))
(out/'job.json').write_text(json.dumps(job, indent=2)+'\n', encoding='utf-8')
(out/'manifest.json').write_text(json.dumps(dict(
    scope='Original diagnostic only; not a game performance result or game qualification',
    source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    rom_sha256=hashlib.sha256(rom).hexdigest(), frames=a.frames,
    features=dict(backend='hybrid' if a.execution == 'default' else a.execution,
                  execution_switch_omitted=a.execution == 'default',
                  native_entries_expected=0, fallback_accounting=a.execution != 'interpreter',
                  persistent_save=False),
    required_reservation=dict(cpu=2, gpu=0, memory_gib=2, workspace_gib=1)), indent=2)+'\n', encoding='utf-8')
print(json.dumps(dict(prepared=True, executed=False, out=str(out), job=str(out/'job.json'))))

"""Generate optimized/original bodies and real opcodes for arithmetic co-sim."""
from pathlib import Path
import random
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from recompiler.v810.analysis import RomImage, FunctionRange
from recompiler.v810.emitter import emit_function, _REGION_OPS

out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
rng=random.Random(0x810)
rom=bytearray(65536)
ranges=[];entries=[]
ops=sorted(_REGION_OPS)
for case in range(8):
    start=0x1000+case*0x200;pc=start;pcs=[]
    order=ops*2;rng.shuffle(order)
    for index,op in enumerate(order):
        pcs.append(0x07000000+pc)
        r1=rng.randrange(32);r2=rng.randrange(32)
        # Include r0 writes and MUL r30 overlap, as well as general aliases.
        if index%7==0:r2=0
        if op in (8,10):r2=30
        if op==18:r1&=15
        struct.pack_into('<H',rom,pc,(op<<10)|(r2<<5)|r1)
        pc+=2
        if op>=0x28:
            struct.pack_into('<H',rom,pc,rng.randrange(65536));pc+=2
    ranges.append(FunctionRange(f'pure_{case}',0x07000000+start,0x07000000+pc))
    entries.append(pcs)
img=RomImage.from_bytes(bytes(rom))
source=['#include "v810_interpreter.h"','#include "interrupts.h"',
        '#include "stub_abort.h"','#include "fntrace.h"']
header=['#include "cpu_state.h"',f'#define REGION_CASES {len(ranges)}',
        f'#define REGION_INSTRUCTIONS {len(entries[0])}']
for fn in ranges:
    source.append(emit_function(img,fn,set()))
    source.append(emit_function(img,fn,set(),native_regions=False).replace('vb_fn_','vb_old_fn_'))
    header.extend(f'void {prefix}{fn.start_pc:08X}(CPUState*);' for prefix in ('vb_fn_','vb_old_fn_'))
for name,values in [('region_start',[fn.start_pc for fn in ranges]),('region_end',[fn.end_pc for fn in ranges])]:
    header.append(f'static const uint32_t {name}[]={{'+','.join(f'0x{v:08X}u' for v in values)+'};')
header.append('static const uint32_t region_pcs[REGION_CASES][REGION_INSTRUCTIONS]={'+
              ','.join('{'+','.join(f'0x{pc:08X}u' for pc in pcs)+'}' for pcs in entries)+'};')
for name,prefix in [('region_new','vb_fn_'),('region_old','vb_old_fn_')]:
    header.append(f'static void (*const {name}[])(CPUState*)={{'+
                  ','.join(f'{prefix}{fn.start_pc:08X}' for fn in ranges)+'};')
(out/'regions.c').write_text('\n'.join(source),encoding='utf-8')
(out/'regions.h').write_text('\n'.join(header),encoding='utf-8')
(out/'regions.rom').write_bytes(rom)

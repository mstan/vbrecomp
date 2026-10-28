"""Generate a collision-heavy full-address dispatch fixture, without game code."""
from pathlib import Path
import random
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from recompiler.v810.analysis import FunctionRange
from recompiler.v810.emitter import emit_dispatch_c, emit_header

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
rng = random.Random(0xD15810)
pcs = {0, 2, 0x07000000, 0x87000000, 0xFFFFFFFE}
pcs.update(rng.randrange(1 << 31) * 2 for _ in range(2048))
pcs = sorted(pcs)
fns = [FunctionRange(f'route_{i}', pc, pc + 2) for i, pc in enumerate(pcs)]
(out/'dispatch_fixture.h').write_text(emit_header(fns, 'dispatch_fixture'), encoding='utf-8')
source = emit_dispatch_c(fns, 'dispatch_fixture')
source += '\n' + '\n'.join(
    f'void vb_fn_{pc:08X}(CPUState* cpu) {{ fixture_execute(cpu, {i}); }}'
    for i, pc in enumerate(pcs))
(out/'dispatch_fixture.c').write_text(source, encoding='utf-8')

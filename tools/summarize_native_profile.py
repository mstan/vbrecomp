"""Group perturbed nearest-symbol samples without redistributing unknown costs."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('profile',type=Path)
p.add_argument('output',type=Path)
args=p.parse_args()
source=json.loads(args.profile.read_text())
patterns=[
    ('generated_guest_code',r'vb_fn_'),
    ('native_dispatch',r'vb_dispatch|vb_native_lookup'),
    ('branch_policy',r'vb_branch_predicate|vb_branch_rule'),
    ('interpreter_extended',r'vb_interpreter|bitstring|fp_input|fp_flags'),
    ('bus_memory',r'vb_read|vb_write|raw_read|raw_write|bus_begin|bus_end|rom_off|observe_write'),
    ('scheduler_devices',r'vb_devices|vb_irq|vb_interrupt|vb_timer|vb_input'),
    ('viewport_replay',r'vb_viewport|tile_row|background|objects|extra_sprites|outside_cell|pixel'),
    ('vip_rendering',r'vb_vip|vip_draw|draw_bg|draw_obj|draw_affine|framebuffer_column|calculate_brt|source_texel|vb_red_lut'),
    ('material_color',r'PreparedColors|renderEPK13VbRenderFrame|color.*render'),
    ('renderer_copies',r'vb_renderer'),
    ('audio',r'vsu_|vb_vsu|Blip_|blip_|ring_push'),
    ('history_state',r'vb_state|vb_rewind|rbe_snap|state_capture|state_restore|StateCodec'),
    ('fingerprinting',r'vb_wram|vb_sha256|vb_crc32'),
    ('host_memory',r'memcpy|memset|memmove|malloc|calloc|realloc|free'),
    ('host_ui',r'ImGui|recomp_|SDL_|LNG_|lng_'),
]
groups=defaultdict(lambda:dict(samples=0,functions=[]))
for row in source['functions']:
    name=row['name']
    group='external_unattributed' if name.startswith(('DLL:','unresolved-')) else next(
        (label for label,pattern in patterns if re.search(pattern,name)),'other_executable')
    groups[group]['samples']+=row['samples'];groups[group]['functions'].append(row)
for row in groups.values():row['percent']=100*row['samples']/source['samples']
result=dict(scope='Perturbed main-thread nearest-symbol samples, including startup; not throughput or inclusive stacks',
    profile=str(args.profile.resolve()),exe_sha256=source['exe_sha256'],samples=source['samples'],sample_errors=source['sample_errors'],
    groups=dict(sorted(groups.items(),key=lambda pair:-pair[1]['samples'])))
args.output.write_text(json.dumps(result,indent=2))
print(json.dumps({k:dict(samples=v['samples'],percent=round(v['percent'],2)) for k,v in result['groups'].items()},indent=2))

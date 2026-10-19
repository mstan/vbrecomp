"""ROM-bound original-loader surveys, compared in separate processes."""
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import time
import tomllib
import zipfile
from cosim import Runner, compare_snapshots

PLANES = ['cpu','wram','sram','vram','vip','devices','video','video_right','audio']
def number(value): return int(value,0) if isinstance(value,str) else int(value)
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def port():
    with socket.socket() as s:
        s.bind(('127.0.0.1',0));return s.getsockname()[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('runtime','rom','suite','out'): p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--mod',type=Path,action='append',default=[])
    p.add_argument('--case',action='append',default=[])
    p.add_argument('--allow-fallback',action='store_true')
    p.add_argument('--matched-mods',action='store_true',help='Apply the same simulation-changing mods to both processes')
    p.add_argument('--compare-presented',action='store_true',help='Also require identical enhanced output surfaces')
    p.add_argument('--control-runtime',type=Path,help='Optional older runtime for regression comparisons')
    p.add_argument('--control-mode',choices=['hybrid','interpreter'],default='interpreter')
    p.add_argument('--aspect',choices=['adaptive','16:9','21:9','32:9'],default='32:9')
    p.add_argument('--require-branch-changes',action='store_true',help='Reject a simulation-extension survey that never changes an original branch decision')
    args=p.parse_args()
    spec=json.loads(args.suite.read_text())
    if digest(args.rom)!=spec['rom_sha256']:p.error('Unsupported cartridge identity')
    cases=[c for c in spec['cases'] if not args.case or c['id'] in args.case]
    if not cases or any(not re.fullmatch(r'[A-Za-z0-9_-]+',c['id']) for c in cases):p.error('Invalid case selection')
    args.out=args.out.resolve()
    os.environ.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy')
    extra=[]
    for package in args.mod:
        with zipfile.ZipFile(package) as z:manifest=tomllib.loads(z.read('manifest.toml').decode())
        extra+=['--install-mod',str(package.resolve())]
        for feature in manifest.get('feature',[]):extra+=['--enable-mod',manifest['id']+':'+feature['id']]
        for option in manifest.get('option',[]):
            if option['id']=='aspect':extra+=['--set-mod-option',manifest['id']+':'+option['feature']+':aspect='+args.aspect]
    planes=PLANES+(['presented'] if args.compare_presented else [])
    control=(args.control_runtime or args.runtime).resolve()
    report=dict(runtime_sha256=digest(args.runtime),control_runtime_sha256=digest(control),control_mode=args.control_mode,rom_sha256=digest(args.rom),suite_sha256=digest(args.suite),mods={str(m):digest(m) for m in args.mod},matched_mods=args.matched_mods,allow_fallback=args.allow_fallback,planes=planes,cases=[])
    args.out.mkdir(parents=True,exist_ok=False)
    def save(): (args.out/'report.json').write_text(json.dumps(report,indent=2)+'\n',newline='\n')
    def run_steps(pair,steps,case):
        for event in steps:
            action=event['action']
            if action=='advance':
                for runner in pair:
                    runner.client.command('set_input',pad=number(event.get('pad',0)));left=number(event['frames'])
                    if left<1:raise ValueError('Positive frame count required')
                    while left:
                        count=min(left,120);runner.client.run_frames(count,timeout=60);left-=count
            elif action=='anchor':
                pc=number(event['pc'])
                for runner in pair:
                    runner.client.command('set_input',pad=0);runner.client.command('breakpoint',pc=pc);runner.client.command('continue');deadline=time.monotonic()+60
                    while number(runner.client.command('get_registers')['pc'])!=pc:
                        if time.monotonic()>deadline:raise TimeoutError('Original controller anchor')
                        time.sleep(.005)
            elif action=='write':
                addr=number(event['addr']);value=number(event['value']);size=number(event.get('size',1))
                if size not in (1,2,4) or not 0x05000000<=addr<=0x05010000-size or not 0<=value<1<<(size*8):raise ValueError('Invalid WRAM assist')
                for runner in pair:
                    for i in range(size):runner.client.command('write_ram',addr=addr+i,val=(value>>(8*i))&255)
            elif action in ('pc_set','set_gpr','breakpoint'):
                for runner in pair:runner.client.command(action,**{k:number(v) for k,v in event.items() if k!='action'})
            elif action=='capture':
                label=event['label']
                if not re.fullmatch(r'[A-Za-z0-9_-]+',label):raise ValueError('Invalid capture label')
                frames=[r.client.command('frame')['frame'] for r in pair];assert frames[0]==frames[1],frames
                snapshots=[r.snapshot(frames[0],planes) for r in pair]
                differences=compare_snapshots(*snapshots)
                row=dict(label=label,frame=frames[0],differences=differences,stats=pair[0].client.command('execution_stats'),viewport=pair[0].client.command('viewport_state'),checks=[])
                for expect in event.get('expect',[]):
                    address=number(expect['addr']);size=number(expect.get('size',4));value=int.from_bytes(pair[0].memory(address,size),'little');allowed=[number(v) for v in expect['values']];row['checks'].append(dict(address=address,value=value,allowed=allowed));assert value in allowed,row
                shot=args.out/(case['id']+'-'+label+'.png');pair[0].client.command('screenshot',eye=0,path=shot.as_posix(),presented=1)
                if event.get('visible'):
                    from _imgio import load_png
                    _,_,pixels=load_png(shot);assert any(v&0xffffff for v in pixels),'Blank scene'
                row['state_hashes']=[{k:hashlib.sha256(v if isinstance(v,bytes) else json.dumps(v,sort_keys=True).encode()).hexdigest() for k,v in state.items()} for state in snapshots]
                case['checkpoints'].append(row);save();assert not differences,row
            else:raise ValueError('Unknown action '+action)
    with ExitStack() as outer:
        pair=None
        for index,plan in enumerate(cases):
            with ExitStack() as inner:
                fresh=pair is None or spec.get('reset_between_cases',True)
                if fresh:
                    stack=inner if spec.get('reset_between_cases',True) else outer
                    pair=[stack.enter_context(Runner(exe,args.rom.resolve(),mode,port(),args.out/(plan['id']+'-'+label),extra=extra if label=='native' or args.matched_mods else ())) for label,mode,exe in [('native','hybrid',args.runtime.resolve()),('control',args.control_mode,control)]]
                    for runner in pair:runner.client.socket.settimeout(60);runner.client.command('viewport_state',window_width=1280,window_height=360)
                case=dict(id=plan['id'],kind=plan.get('kind','sample'),checkpoints=[]);report['cases'].append(case)
                if fresh:run_steps(pair,spec.get('initial',[]),case)
                run_steps(pair,plan['steps'],case)
                coverage=pair[0].client.command('fallback_coverage');stats=pair[0].client.command('execution_stats')
                if 'branch_rules' in pair[0].client.command('capabilities')['commands']:
                    case['branch_rules']=[r.client.command('branch_rules') if 'branch_rules' in r.client.command('capabilities')['commands'] else None for r in pair]
                assert sum(e['hits'] for e in coverage['entries'])+coverage['overflow_hits']==stats['fallback']
                case.update(stats=stats,fallback_coverage=coverage);save()
                case['control_stats']=pair[1].client.command('execution_stats');save()
                assert not coverage['overflow_hits'],'Incomplete fallback-PC map'
                assert args.allow_fallback or not stats['fallback'],stats
                assert case['checkpoints'],'Case has no audited checkpoints'
                print(plan['id'],'PASS, fallback',stats['fallback'],flush=True)
    if args.require_branch_changes:
        rules=report['cases'][-1].get('branch_rules',[None])[0]
        assert rules and rules['count'] and any(r['changed'] for r in rules['rules']),'Survey did not exercise widened branch decisions'
    report['passed']=True;save()
if __name__=='__main__':main()

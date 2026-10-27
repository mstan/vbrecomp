"""Compare matched stock/enhanced games, both eyes and current-build restore.

Inputs are ordinary committed controller routes. No guest progress flags or
owner saves are changed. Each process owns a disposable profile.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
from cosim import Runner, compare_snapshots


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('reference', 'candidate', 'rom', 'route', 'out'):
        p.add_argument('--' + n, type=Path, required=True)
    p.add_argument('--color', type=Path)
    p.add_argument('--wide', type=Path)
    p.add_argument('--port', type=int, default=16720)
    args = p.parse_args()
    args.out = args.out.resolve(); args.out.mkdir(parents=True, exist_ok=False)
    report = dict(passed=False, checks=[], checkpoints=[], identities={n: dict(path=str(v.resolve()),
        sha256=hashlib.sha256(v.read_bytes()).hexdigest()) for n in ('reference','candidate','rom','route','color','wide')
        if (v := getattr(args, n))})
    def save(): (args.out / 'report.json').write_text(json.dumps(report, indent=2))
    extra=[]
    for archive,feature in ((args.color,'wario-land-color:color'),(args.wide,'wario-land-widescreen:widescreen')):
        if archive: extra += ['--install-mod',str(archive.resolve()),'--enable-mod',feature]
    if args.wide: extra += ['--set-mod-option','wario-land-widescreen:widescreen:aspect=adaptive']
    def image(r, path, eye):
        r.client.command('screenshot', presented=1, eye=eye, path=path.as_posix())
        return path.read_bytes()
    try:
        with ExitStack() as stack:
            runners=[stack.enter_context(Runner(exe,args.rom,'hybrid',args.port+i,args.out/name,extra))
                     for i,(name,exe) in enumerate((('reference',args.reference),('candidate',args.candidate)))]
            for r in runners:
                r.client.socket.settimeout(90)
                if args.wide: r.client.command('viewport_state',window_width=1400,window_height=600)
            frame=0
            planes='cpu,wram,sram,vram,vip,devices,video,video_right,audio'.split(',')
            for step in json.loads(args.route.read_text()):
                for r in runners:
                    r.client.command('set_input',pad=step.get('pad',0))
                    r.client.run_frames(step['frames'],timeout=90)
                frame+=step['frames']
                if not step.get('label'): continue
                snapshots=[r.snapshot(frame,planes) for r in runners]
                differences=compare_snapshots(*snapshots)
                assert not differences,(frame,differences)
                for eye in (0,1):
                    a,b=[image(r,r.directory/f'frame-{frame}-presented-{eye}.png',eye) for r in runners]
                    assert a==b,(frame,eye,'presented pixels differ')
                report['checkpoints'].append(dict(frame=frame,label=step['label'],passed=True));save()
            candidate=runners[1]
            state=args.out/'candidate.state'
            candidate.client.command('rewind_enable',value=4)
            candidate.client.command('save_state',path=state.as_posix())
            candidate.client.run_frames(2,timeout=90)
            before=candidate.snapshot(frame+2,planes)
            before_images=[image(candidate,args.out/f'before-{eye}.png',eye) for eye in (0,1)]
            candidate.client.command('load_state',path=state.as_posix())
            candidate.client.run_frames(2,timeout=90)
            after=candidate.snapshot(frame+2,planes)
            assert not compare_snapshots(before,after),'current-build replay differs'
            for eye in (0,1): assert before_images[eye]==image(candidate,args.out/f'after-{eye}.png',eye)
            report['checks'].append('candidate current-build restore/replay: CPU, memory, devices, PCM and both presented eyes')
            candidate.client.command('rewind',value=frame)
            candidate.client.run_frames(2,timeout=90)
            assert not compare_snapshots(before,candidate.snapshot(frame+2,planes)),'rewind replay differs'
            for eye in (0,1): assert before_images[eye]==image(candidate,args.out/f'rewind-{eye}.png',eye)
            report['checks'].append('candidate current-build rewind/replay preserves simulation, audio and both presented eyes')
            runners[0].client.run_frames(2,timeout=90)
            if args.wide:
                for width,height in ((1600,900),(2100,900),(3200,900),(500,900),(1400,600)):
                    states=[r.client.command('viewport_state',window_width=width,window_height=height) for r in runners]
                    assert states[0]['width']==states[1]['width']
                    for eye in (0,1):
                        images=[image(r,r.directory/f'resize-{width}-{height}-{eye}.png',eye) for r in runners]
                        assert images[0]==images[1],(width,height,eye,'adaptive resize pixels differ')
                report['checks'].append('adaptive geometry and pixels match at native, wide and ultrawide dimensions for both eyes')
            report['execution']=[r.client.command('execution_stats') for r in runners]
            report['passed']=True;save()
    except Exception as error:
        report['error']=repr(error);save();raise
    print(json.dumps(dict(passed=True,checkpoints=len(report['checkpoints']),checks=report['checks'])))


if __name__=='__main__': main()

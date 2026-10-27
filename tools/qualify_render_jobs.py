"""Compare matched stock/enhanced games, both eyes and current-build restore.

Inputs are ordinary committed controller routes. No guest progress flags or
owner saves are changed. Each process owns a disposable profile.
"""
import argparse
from contextlib import ExitStack
import hashlib
import json
from pathlib import Path
import sys
from cosim import Runner, compare_snapshots


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for n in ('reference', 'candidate', 'rom', 'route', 'out'):
        p.add_argument('--' + n, type=Path, required=True)
    p.add_argument('--color', type=Path)
    p.add_argument('--wide', type=Path)
    p.add_argument('--color-feature', default='wario-land-color:color')
    p.add_argument('--game-argument', action='append', default=[])
    p.add_argument('--every-segment', action='store_true')
    p.add_argument('--canonical-digest', action='store_true', help='Compare complete canonical payload, including title-owned simulation spans')
    p.add_argument('--port', type=int, default=16720)
    p.add_argument('--final-only', action='store_true', help='Reuse prior route comparisons; check final state, lifecycle and geometry')
    args = p.parse_args()
    args.out = args.out.resolve(); args.out.mkdir(parents=True, exist_ok=False)
    report = dict(passed=False, final_only=args.final_only, checks=[], checkpoints=[], identities={n: dict(path=str(v.resolve()),
        sha256=hashlib.sha256(v.read_bytes()).hexdigest()) for n in ('reference','candidate','rom','route','color','wide')
        if (v := getattr(args, n))})
    def save(): (args.out / 'report.json').write_text(json.dumps(report, indent=2))
    report['features']=dict(color_feature=args.color_feature if args.color else None,wide=bool(args.wide),game_arguments=args.game_argument)
    report.update(command=sys.argv,harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),canonical_digest=args.canonical_digest)
    extra=list(args.game_argument)
    for archive,feature in ((args.color,args.color_feature),(args.wide,'wario-land-widescreen:widescreen')):
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
            steps=json.loads(args.route.read_text())
            for step_index,step in enumerate(steps):
                for r in runners:
                    r.client.command('set_input',pad=step.get('pad',0))
                    r.client.run_frames(step['frames'],timeout=90)
                frame+=step['frames']
                if args.final_only and step_index!=len(steps)-1: continue
                if not step.get('label') and not args.every_segment and step_index!=len(steps)-1: continue
                snapshots=[r.snapshot(frame,planes) for r in runners]
                differences=compare_snapshots(*snapshots)
                assert not differences,(frame,differences)
                if args.canonical_digest:
                    digests=[r.client.command('state_digest')['digest'] for r in runners]
                    assert digests[0]==digests[1],(frame,'canonical simulation payload differs',digests)
                for eye in (0,1):
                    a,b=[image(r,r.directory/f'frame-{frame}-presented-{eye}.png',eye) for r in runners]
                    assert a==b,(frame,eye,'presented pixels differ')
                report['checkpoints'].append(dict(frame=frame,label=step.get('label',f'segment-{step_index+1}'),passed=True));save()
            candidate=runners[1]
            reference_state=args.out/'reference.state'
            runners[0].client.command('save_state',path=reference_state.as_posix())
            state=args.out/'candidate.state'
            candidate.client.command('rewind_enable',value=16)
            candidate.client.command('save_state',path=state.as_posix())
            # Restoring discards the diagnostic PCM probe. Refill beyond its
            # absolute 4096-sample comparison window and 1024-sample margin.
            replay_frames=8
            candidate.client.run_frames(replay_frames,timeout=90)
            before=candidate.snapshot(frame+replay_frames,planes)
            before_digest=candidate.client.command('state_digest')['digest'] if args.canonical_digest else None
            before_images=[image(candidate,args.out/f'before-{eye}.png',eye) for eye in (0,1)]
            candidate.client.command('load_state',path=state.as_posix())
            # File loads intentionally clear history; start the rewind check
            # from the restored state rather than expecting old entries.
            candidate.client.command('rewind_enable',value=16)
            candidate.client.run_frames(replay_frames,timeout=90)
            after=candidate.snapshot(frame+replay_frames,planes)
            assert not compare_snapshots(before,after),'current-build replay differs'
            if args.canonical_digest: assert candidate.client.command('state_digest')['digest']==before_digest
            for eye in (0,1): assert before_images[eye]==image(candidate,args.out/f'after-{eye}.png',eye)
            report['checks'].append('candidate current-build restore/replay: CPU, memory, devices, PCM and both presented eyes')
            candidate.client.command('rewind',value=frame)
            candidate.client.run_frames(replay_frames,timeout=90)
            assert not compare_snapshots(before,candidate.snapshot(frame+replay_frames,planes)),'rewind replay differs'
            if args.canonical_digest: assert candidate.client.command('state_digest')['digest']==before_digest
            for eye in (0,1): assert before_images[eye]==image(candidate,args.out/f'rewind-{eye}.png',eye)
            report['checks'].append('candidate current-build rewind/replay preserves simulation, audio and both presented eyes')
            runners[0].client.run_frames(replay_frames,timeout=90)
            reference_after=runners[0].snapshot(frame+replay_frames,planes)
            assert not compare_snapshots(before,reference_after),'reference/candidate continuation differs'
            reference_digest=runners[0].client.command('state_digest')['digest'] if args.canonical_digest else None
            if args.wide:
                for width,height in ((1600,900),(2100,900),(3200,900),(500,900),(1400,600)):
                    states=[r.client.command('viewport_state',window_width=width,window_height=height) for r in runners]
                    assert states[0]['width']==states[1]['width']
                    for eye in (0,1):
                        images=[image(r,r.directory/f'resize-{width}-{height}-{eye}.png',eye) for r in runners]
                        assert images[0]==images[1],(width,height,eye,'adaptive resize pixels differ')
                report['checks'].append('adaptive geometry and pixels match at native, wide and ultrawide dimensions for both eyes')
            report['execution']=[r.client.command('execution_stats') for r in runners]
            reference_images=[image(runners[0],args.out/f'reference-after-{eye}.png',eye) for eye in (0,1)]
        # A fresh process has no title observer/cache residue from earlier
        # gameplay. Loading must preserve the inputs for future captured frames.
        for index,(name,exe,saved,expected,expected_images,digest) in enumerate((
            ('reference',args.reference,reference_state,reference_after,reference_images,reference_digest),
            ('candidate',args.candidate,state,before,before_images,before_digest))):
            with Runner(exe,args.rom,'hybrid',args.port+2+index,args.out/(name+'-fresh'),extra) as fresh:
                fresh.client.socket.settimeout(90)
                if args.wide:fresh.client.command('viewport_state',window_width=1400,window_height=600)
                fresh.client.command('set_input',pad=steps[-1].get('pad',0))
                fresh.client.command('load_state',path=saved.as_posix())
                fresh.client.run_frames(replay_frames,timeout=90)
                assert not compare_snapshots(expected,fresh.snapshot(frame+replay_frames,planes)),(name,'fresh-process simulation differs')
                if args.canonical_digest:assert fresh.client.command('state_digest')['digest']==digest
                for eye in (0,1):assert expected_images[eye]==image(fresh,args.out/f'{name}-fresh-{eye}.png',eye),(name,eye,'fresh-process presentation differs')
            report['checks'].append(name+' fresh-process restore and continuation preserve simulation, PCM and both eyes');save()
        report['passed']=True;save()
    except Exception as error:
        report['error']=repr(error);save();raise
    print(json.dumps(dict(passed=True,checkpoints=len(report['checkpoints']),checks=report['checks'])))


if __name__=='__main__': main()

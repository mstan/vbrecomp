"""Exercise the actual SDL event pump, slot files and rewind menu, muted.

Keyboard/controller-button events are injected into SDL's queue, not the OS.
This qualifies host dispatch, not physical device drivers or held OS keys.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import struct
import time
from debug_client import DebugClient
from _imgio import load_png


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runtime',type=Path,required=True)
    p.add_argument('--rom',type=Path,required=True)
    p.add_argument('--route',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--mod-archive',type=Path)
    p.add_argument('--configured-history',action='store_true',help='Exercise persisted cadence and default enablement instead of CLI history flags')
    p.add_argument('--port',type=int,default=15874)
    a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
    profile=a.out/'profile-é-試験';profile.mkdir()
    report={'passed':False,'checks':[],'runtime_sha256':hashlib.sha256(a.runtime.read_bytes()).hexdigest(),
            'scope':'real SDL queue and host UI, dummy devices; physical drivers/held OS keys unverified'}
    process=client=None
    def check(name,value):
        report['checks'].append({'name':name,'passed':bool(value)})
        if not value:raise AssertionError(name)
    try:
        with socket.socket() as probe:
            if os.name=='nt':probe.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
            probe.bind(('127.0.0.1',a.port))
        settings='fullscreen 0\naudio 1\nvolume 0\nscale 1\n'
        if a.configured_history:settings+='rewind_depth 8\nrewind_interval 3\n'
        (profile/'settings.cfg').write_text(settings)
        command=[str(a.runtime.resolve()),'--rom',str(a.rom.resolve()),'--paused','--no-launcher',
                 '--port',str(a.port),'--no-save','--config',str(profile/'settings.cfg'),
                 '--mods-dir',str(profile/'mods'),'--state-file',str(profile/'quick.state')]
        if not a.configured_history:command+=['--rewind-frames','8','--rewind-interval','3']
        if a.mod_archive:command+=['--install-mod',str(a.mod_archive.resolve()),'--enable-mod','zero-racers.full-color:full-color']
        env={k:v for k,v in os.environ.items() if not k.startswith('VBRECOMP_')}
        env.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy',SDL_RENDER_DRIVER='software',
                   SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT='0xffff/0xffff')
        with (a.out/'stdout.log').open('wb') as out,(a.out/'stderr.log').open('wb') as err:
            process=subprocess.Popen(command,cwd=a.out,env=env,stdout=out,stderr=err,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
            deadline=time.monotonic()+15
            while client is None:
                if process.poll() is not None:raise RuntimeError('UI runtime exited during startup')
                try:client=DebugClient(a.port)
                except OSError:
                    if time.monotonic()>deadline:raise
                    time.sleep(.02)
            def ui():return client.command('state_ui')
            def wait(predicate):
                deadline=time.monotonic()+5
                while time.monotonic()<deadline:
                    value=ui()
                    if predicate(value):return value
                    time.sleep(.02)
                raise TimeoutError('UI transition did not complete: '+str(ui()))
            def key(scancode,repeat=0):
                client.command('host_input',device=0,value=scancode,pressed=1,repeat=repeat)
                client.command('host_input',device=0,value=scancode,pressed=0)
                time.sleep(.05)
            def button(number):
                client.command('host_input',device=1,value=number,pressed=1)
                client.command('host_input',device=1,value=number,pressed=0)
                time.sleep(.05)
            def snapshot(name):
                path=a.out/(name+'.state');client.command('save_state',path=path.as_posix());return path.read_bytes()
            def picture(name):
                path=a.out/(name+'.png');client.command('screenshot',host=1,path=path.as_posix());return load_png(path)
            def slotpath(n):return next((profile/'states').glob('*/slot-%02d.state'%n))

            remaining=2600
            for segment in json.loads(a.route.read_text()):
                count=min(remaining,segment['frames'])
                if not count:break
                client.command('set_input',pad=segment.get('pad',0));client.command('replay_frames',frames=count)
                remaining-=count
            client.command('set_input',pad=0);client.run_frames(24)
            frame=client.command('frame')['frame'];original=snapshot('before-browser')
            key(64);wait(lambda x:x['mode']==1) # F7
            check('F7 opens graphical twelve-slot browser',picture('slots-empty')[:2]==(768,448))
            key(64,repeat=1)
            check('opening key repeat does not close the menu',ui()['mode']==1)
            client.command('continue');time.sleep(.12)
            check('slot browser itself holds a running guest',client.command('frame')['frame']==frame)
            client.command('pause')
            time.sleep(.1)
            check('slot browsing freezes full machine and presentation state',
                  client.command('frame')['frame']==frame and snapshot('while-browser')==original)
            key(22);wait(lambda x:x['result']==1) # S
            first=slotpath(1)
            check('S saves the selected snapshot and bound thumbnail in Unicode profile',first.read_bytes()==original and Path(str(first)+'.thumb').is_file())
            left=a.out/'saved-left-eye.png';client.command('screenshot',eye=0,presented=1,path=left.as_posix())
            width,height,pixels=load_png(left);preview=Path(str(first)+'.thumb').read_bytes()
            expected=[pixels[(y*height//75)*width+x*width//128]&0xffffff for y in range(75) for x in range(128)]
            actual=[value[0]&0xffffff for value in struct.iter_unpack('<I',preview[120:])]
            check('saved thumbnail matches the authoritative left-eye presentation',preview[8:120]==original[:112] and actual==expected)
            picture('slots-saved')
            key(46);button(2) # '=' selects12; controller X saves
            check('twelfth slot and controller Save work',ui()['slot']==11 and slotpath(12).is_file())
            key(32);key(40) # 3, Enter
            check('empty-slot Load rejects without closing or mutating state',ui()['result']==-1 and ui()['mode']==1 and snapshot('empty-reject')==original)
            key(33);blocked=first.with_name('slot-04.state');blocked.mkdir();key(22)
            check('unwritable slot destination is reported without damaging machine or slot',ui()['result']==-1 and blocked.is_dir() and snapshot('save-reject')==original)
            key(64);wait(lambda x:x['mode']==0);client.run_frames(6)
            key(64);key(30);button(0) # controller A loads slot1
            wait(lambda x:x['mode']==0)
            check('controller Load restores complete current-build state',client.command('frame')['frame']==frame and snapshot('after-load')==original)
            client.run_frames(24)
            live=snapshot('before-rewind');current=client.command('frame')['frame']
            key(65);wait(lambda x:x['mode']==2) # F8
            check('F8 opens a populated graphical filmstrip',ui()['count']>=2 and picture('rewind')[:2]==(768,448))
            client.command('continue');time.sleep(.12)
            check('filmstrip itself holds a running guest',client.command('frame')['frame']==current)
            client.command('pause')
            before_scrub=ui()['selected_frame'];key(80);selected=ui()['selected_frame'] # left
            check('configured capture interval produces exact frame spacing',before_scrub-selected==3)
            check('scrubbing selects older history without restoring it',selected<current and snapshot('while-rewind')==live)
            time.sleep(.1)
            check('rewind browsing freezes guest execution',client.command('frame')['frame']==current)
            button(1);wait(lambda x:x['mode']==0)
            check('controller Cancel preserves the complete live state',snapshot('after-cancel')==live)
            key(65);key(80);target=ui()['selected_frame'];key(40);wait(lambda x:x['mode']==0)
            check('Select commits the exact selected rewind frame',client.command('frame')['frame']==target and ui()['result']==1)
            key(65)
            for _ in range(12):key(79) # right, clamp at new end
            check('committed rewind drops the abandoned future',ui()['selected_frame']<=target)
            client.command('reset');wait(lambda x:x['count']<=1)
            check('reset invalidates stale filmstrip previews',ui()['count']<=1)
            key(41);key(64);key(30)
            damaged=bytearray(first.read_bytes());damaged[0]^=1;first.write_bytes(damaged)
            before=snapshot('before-invalid-slot');key(40)
            check('damaged slot is rejected atomically through the GUI',ui()['result']==-1 and ui()['mode']==1 and snapshot('after-invalid-slot')==before)
            picture('slots-rejected');key(41)
            check('Escape closes menu without quitting the game',ui()['mode']==0 and process.poll() is None)
            # Reopen re-reads sidecars: altered snapshot identity cannot retain a stale thumbnail.
            key(64);picture('slots-reopened');key(41)
            report['passed']=True
    except Exception as error:report['error']=str(error)
    finally:
        if client:
            try:client.command('quit')
            except Exception:pass
            client.close()
        if process:
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=5)
            if process.returncode:report['passed']=False;report['process_exit']=process.returncode
        (a.out/'report.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))
    return 0 if report['passed'] else 1


if __name__=='__main__':raise SystemExit(main())

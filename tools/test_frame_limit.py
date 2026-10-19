"""Exercise finite headless execution, including production without TCP tools."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--runtime',type=Path,required=True)
p.add_argument('--rom',type=Path,help='Omit only for the generic interpreter host')
args=p.parse_args();environment=dict(os.environ,SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy')
options=dict(creationflags=subprocess.CREATE_NO_WINDOW) if os.name=='nt' else {}
with tempfile.TemporaryDirectory(prefix='vb frame limit ') as temp:
    root=Path(temp);rom=args.rom.resolve() if args.rom else root/'synthetic.vb'
    if not args.rom:
        fixture=bytearray(65536);fixture[-16:-14]=(0x1a<<10).to_bytes(2,'little');rom.write_bytes(fixture)
    base=[str(args.runtime.resolve()),'--rom',str(rom),'--headless','--no-save','--config',str(root/'profile.cfg'),'--mods-dir',str(root/'mods')]
    if not args.rom:base+=['--execution','interpreter']
    for count in ('0','-1','1.5','1000001','184467440737095516160'):
        result=subprocess.run([*base,'--frames',count],cwd=root,env=environment,capture_output=True,text=True,timeout=60,**options)
        assert result.returncode==2,(count,result.returncode,result.stderr)
    for count in (1,3):
        result=subprocess.run([*base,'--frames',str(count)],cwd=root,env=environment,capture_output=True,text=True,timeout=60,**options)
        assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
        match=re.search(r'frame limit reached frame=(\d+) native=(\d+) fallback=(\d+)',result.stdout)
        assert match and int(match[1])==count,(count,result.stdout)
        if args.rom:assert int(match[2])>0 and int(match[3])==0,match[0]
    print('PASS: invalid limits rejected; exact guest frames 1/3 exit cleanly without debugger control')

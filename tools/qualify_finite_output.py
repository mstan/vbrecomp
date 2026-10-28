"""Check finite report success and explicit failure for an unwritable stdout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('exe','rom','out'):p.add_argument('--'+n,type=Path,required=True)
    a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
    env={k:v for k,v in os.environ.items() if not k.startswith('VBRECOMP_')}
    env.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy')
    results=dict(passed=False,exe_sha256=hashlib.sha256(a.exe.read_bytes()).hexdigest(),checks=[])
    def save():(a.out/'report.json').write_text(json.dumps(results,indent=2))
    save()
    for label in ('normal','read-only-output'):
        directory=a.out/label;directory.mkdir()
        config=directory/'settings.cfg';config.write_text('fullscreen 0\naudio 1\nvolume 0\n')
        command=[str(a.exe.resolve()),'--rom',str(a.rom.resolve()),'--benchmark','1','--no-save',
                 '--config',str(config.resolve()),'--mods-dir',str((directory/'mods').resolve())]
        target=directory/'stdout.txt'
        if label!='normal':target.write_bytes(b'protected-output-fixture')
        with target.open('wb' if label=='normal' else 'rb') as out:
            result=subprocess.run(command,cwd=directory,env=env,stdout=out,stderr=subprocess.PIPE,timeout=45,
                                  creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
        (directory/'stderr.txt').write_bytes(result.stderr)
        if label=='normal':
            metrics=[json.loads(s.split(' ',1)[1]) for s in target.read_text().splitlines() if s.startswith('VBRECOMP_BENCHMARK ')]
            assert result.returncode==0 and len(metrics)==1 and metrics[0]['frames']==1
        else:
            assert result.returncode==9 and b'could not be written' in result.stderr
            assert target.read_bytes()==b'protected-output-fixture'
        results['checks'].append(dict(label=label,exit_code=result.returncode,passed=True));save()
    results['passed']=True;save();print(json.dumps(results))


if __name__=='__main__':main()

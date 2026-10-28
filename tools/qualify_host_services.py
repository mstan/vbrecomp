"""Confirm staged host-service policies before the full shared-package gate.

The one-frame probes establish binary policy, not performance. Run this command
inside benchmark_qualify_window.py so probes and route checks own one host slot.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import sys


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('reference','candidate','rom','out'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--reference-service-cycles',type=int,default=0)
    p.add_argument('--candidate-service-cycles',type=int,default=20000)
    a,extra=p.parse_known_args();a.out.mkdir(parents=True,exist_ok=False)
    report=dict(passed=False,scope='Binary policy preflight; no throughput claim',probes=[],
                harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    def save():(a.out/'preflight.json').write_text(json.dumps(report,indent=2))
    save();environment=os.environ.copy();environment.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy')
    for label,exe,expected in [('reference',a.reference,a.reference_service_cycles),('candidate',a.candidate,a.candidate_service_cycles)]:
        directory=a.out/(label+'-probe');directory.mkdir()
        config=directory/'settings.cfg';config.write_text('fullscreen 0\naudio 1\nvolume 0\nscale 1\n')
        with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
        command=[str(exe.resolve()),'--rom',str(a.rom.resolve()),'--measure-runtime','1','--measure-window','384x448',
                 '--no-save','--config',str(config.resolve()),'--mods-dir',str((directory/'mods').resolve()),'--port',str(port)]
        result=subprocess.run(command,env=environment,cwd=directory,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=90,
                              creationflags=subprocess.CREATE_NO_WINDOW)
        (directory/'stdout.txt').write_bytes(result.stdout);(directory/'stderr.txt').write_bytes(result.stderr)
        metrics=[json.loads(line.split(' ',1)[1]) for line in result.stdout.decode(errors='replace').splitlines()
                 if line.startswith('VBRECOMP_BENCHMARK ')]
        row=dict(label=label,command=command,exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                 expected_host_service_cycles=expected,exit_code=result.returncode,metric=metrics[0] if len(metrics)==1 else None)
        report['probes'].append(row);save()
        if result.returncode or len(metrics)!=1 or metrics[0].get('host_service_cycles')!=expected:
            raise RuntimeError('Staged host-service policy mismatch: '+label)
    command=[sys.executable,str(Path(__file__).with_name('qualify_render_jobs.py')),
             '--reference',str(a.reference.resolve()),'--candidate',str(a.candidate.resolve()),'--rom',str(a.rom.resolve()),
             '--out',str((a.out/'route').resolve()),*extra]
    report['qualification_command']=command;save()
    report['qualification_exit']=subprocess.run(command,env=environment,creationflags=subprocess.CREATE_NO_WINDOW).returncode
    report['passed']=report['qualification_exit']==0;save();return report['qualification_exit']


if __name__=='__main__':sys.exit(main())

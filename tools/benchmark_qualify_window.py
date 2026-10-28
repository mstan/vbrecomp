"""Serialize a muted finite qualification command with other engine campaigns."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from measure_ab_windows import census
from performance_window_windows import PerformanceWindow,PerformanceWindowBusy


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--receipt',required=True,type=Path)
    p.add_argument('command',nargs=argparse.REMAINDER)
    args=p.parse_args();command=args.command
    if command and command[0]=='--':command=command[1:]
    if not command:p.error('A qualification command is required')
    args.receipt.parent.mkdir(parents=True,exist_ok=True)
    if args.receipt.exists():raise FileExistsError(args.receipt)
    report=dict(passed=False,command=command,started=time.time(),
                harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                scope='Automated correctness qualification; not throughput')
    def save():args.receipt.write_text(json.dumps(report,indent=2))
    save()
    with PerformanceWindow() as lock:
        report['lock_paths']=[str(p) for p in lock.paths]
        report['lock_helper_sha256']=hashlib.sha256(Path(__file__).with_name('performance_window_windows.py').read_bytes()).hexdigest()
        try:lock.acquire()
        except PerformanceWindowBusy as error:
            report['deferred']=str(error);save();return 75
        try:
            state,busy=census();report['census_before']=state
            query=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-Command',
                "Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne "+str(os.getpid())+
                " -and $_.Name -match '^python' -and $_.CommandLine -match 'measure_[\\w-]+[.]py|benchmark[\\w-]*[.]py|profile_native_windows[.]py|run_pair[.]py|run_ab[\\w-]*[.]py|stress[\\w-]*[.]py' } | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress"],
                capture_output=True,text=True,check=True,creationflags=subprocess.CREATE_NO_WINDOW)
            report['other_measurements']=query.stdout.strip()
            if busy or query.stdout.strip():
                report['deferred']=busy or query.stdout.strip();save();return 75
            environment=os.environ.copy()
            environment.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy')
            report['exit_code']=subprocess.run(command,env=environment,stdout=sys.stdout,stderr=sys.stderr,
                creationflags=subprocess.CREATE_NO_WINDOW).returncode
            report['passed']=report['exit_code']==0
            report['census_after'],report['headroom_warnings']=census()
            report['finished']=time.time();save();return report['exit_code']
        finally:
            lock.release()


if __name__=='__main__':sys.exit(main())

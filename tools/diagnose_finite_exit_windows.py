"""Bounded, muted debugger diagnosis of missing finite-runtime results.

Records the VM frame/PC and native exit caller; debugger runs are not timings.
Use benchmark_qualify_window.py to own a coordinated host slot.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','candidate','rom','route','color','out'):p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--cdb',type=Path,default=Path('C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe'))
    a=p.parse_args()
    if os.name!='nt' or ctypes.sizeof(ctypes.c_void_p)!=8 or not a.cdb.is_file():
        raise RuntimeError('This optional diagnostic requires Windows x64 CDB')
    a.out.mkdir(parents=True,exist_ok=False);empty=a.out/'empty-symbols';empty.mkdir()
    environment={k:v for k,v in os.environ.items() if not k.startswith('VBRECOMP_')}
    environment.update(SDL_AUDIODRIVER='dummy',SDL_VIDEODRIVER='dummy',_NT_SYMBOL_PATH=str(empty.resolve()),_NT_ALT_SYMBOL_PATH='')
    # Retain the failed series' affinity; debugger overhead is never a speed result.
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.SetProcessAffinityMask.argtypes=[ctypes.c_void_p,ctypes.c_size_t]
    if not kernel.SetProcessAffinityMask(ctypes.c_void_p(-1),0xc003):raise ctypes.WinError(ctypes.get_last_error())
    report=dict(scope='Debugger diagnostics only; not performance evidence',runs=[])
    def save():(a.out/'report.json').write_text(json.dumps(report,indent=2))
    save()
    for label,exe in [('A1',a.baseline),('B1',a.candidate),('B2',a.candidate),('B3',a.candidate)]:
        directory=a.out/label;directory.mkdir()
        symbols=subprocess.check_output(['C:/msys64/mingw64/bin/nm.exe','-n',str(exe)],text=True)
        (directory/'symbols.txt').write_text(symbols)
        raw=exe.read_bytes();pe=struct.unpack_from('<I',raw,0x3c)[0];preferred=struct.unpack_from('<Q',raw,pe+48)[0]
        found={}
        for line in symbols.splitlines():
            fields=line.split()
            if len(fields)==3 and fields[2] in ('s_bus_cpu','vb_watchdog_stop'):found[fields[2]]=int(fields[0],16)-preferred
        assert len(found)==2
        module=exe.stem;pointer=f'poi({module}+0x{found["s_bus_cpu"]:x})'
        inspect=f'.echo VM_FRAME_PC_CYCLES; dd {pointer}+0x120 L1; dd {pointer}+0x80 L1; dq {pointer}+0x118 L1; k'
        command_file=directory/'commands.txt'
        command_file.write_text('.symopt+0x80000\n'+
            f'bu {module}+0x{found["vb_watchdog_stop"]:x} ".echo WATCHDOG_STOP; {inspect}; g"\n'+
            f'bu ntdll!RtlExitUserProcess ".echo EXIT_CALL; r rcx; {inspect}; .dump /m /o {(directory/"exit.dmp").resolve()}; g"\n'+
            'g\n')
        config=directory/'settings.cfg';config.write_text('fullscreen 0\naudio 1\nvolume 0\nscale 1\n')
        child=[str(exe.resolve()),'--rom',str(a.rom.resolve()),'--measure-runtime','4000','--measure-window','384x448',
               '--benchmark-route',str(a.route.resolve()),'--stereo','--no-save','--config',str(config.resolve()),
               '--mods-dir',str((directory/'mods').resolve()),'--rewind-frames','32','--rewind-interval','6',
               '--install-mod',str(a.color.resolve()),'--enable-mod','zero-racers.full-color:full-color']
        command=[str(a.cdb),'-G','-o','-y',str(empty.resolve()),'-cf',str(command_file.resolve()),*child]
        row=dict(label=label,command=command,exe_sha256=hashlib.sha256(raw).hexdigest(),status='running')
        report['runs'].append(row);save()
        with (directory/'stdout.txt').open('wb') as out,(directory/'stderr.txt').open('wb') as err:
            try:row['exit_code']=subprocess.run(command,cwd=directory,env=environment,stdout=out,stderr=err,timeout=90,
                                               creationflags=subprocess.CREATE_NO_WINDOW).returncode
            except subprocess.TimeoutExpired:row['timeout']=True
        output=(directory/'stdout.txt').read_text(errors='replace')
        lines=[line.strip() for line in output.splitlines()]
        row.update(status='complete',benchmark_tokens=sum(line.startswith('VBRECOMP_BENCHMARK ') for line in lines),
                   watchdog_stops=lines.count('WATCHDOG_STOP'),exit_calls=lines.count('EXIT_CALL'))
        save();print(json.dumps(row),flush=True)
    return 0


if __name__=='__main__':sys.exit(main())

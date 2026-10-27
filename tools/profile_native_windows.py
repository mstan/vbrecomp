"""Sample the main thread of an owned finite Windows runtime workload.

Suspend/GetThreadContext briefly perturbs execution: use this for attribution,
never throughput evidence. DLL-only samples remain unresolved. Artifacts name
the actual executable, image base, symbols, errors and complete child command.
"""
import argparse
import bisect
from collections import Counter
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    symbols_input = p.add_mutually_exclusive_group(required=True)
    symbols_input.add_argument('--nm', type=Path)
    symbols_input.add_argument('--symbols', type=Path, help='Matching nm output prepared on the build host')
    p.add_argument('command', nargs=argparse.REMAINDER)
    args = p.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    args.out.mkdir(parents=True, exist_ok=True)
    exe = Path(command[0]).resolve()
    identity = hashlib.sha256(exe.read_bytes()).hexdigest()
    raw = exe.read_bytes(); pe = struct.unpack_from('<I', raw, 0x3c)[0]
    if struct.unpack_from('<H', raw, pe + 24)[0] != 0x20b:
        raise ValueError('This probe requires a 64-bit Windows PE')
    preferred = struct.unpack_from('<Q', raw, pe + 48)[0]
    symbols = args.symbols.read_text() if args.symbols else subprocess.check_output(
        [str(args.nm), '-n', '--defined-only', str(exe)], text=True)
    (args.out / 'symbols.txt').write_text(symbols)
    entries = []
    for line in symbols.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) == 3 and fields[1] in ('T', 't'):
            entries.append((int(fields[0], 16), fields[2]))
    entries.sort(); addresses = [a for a, _ in entries]
    k = c.WinDLL('kernel32', use_last_error=True)
    k.OpenThread.argtypes = [w.DWORD, w.BOOL, w.DWORD]; k.OpenThread.restype = w.HANDLE
    k.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]; k.CreateToolhelp32Snapshot.restype = w.HANDLE
    k.CloseHandle.argtypes = [w.HANDLE]
    k.SuspendThread.argtypes = [w.HANDLE]; k.SuspendThread.restype = w.DWORD
    k.ResumeThread.argtypes = [w.HANDLE]; k.ResumeThread.restype = w.DWORD
    k.GetThreadContext.argtypes = [w.HANDLE, c.c_void_p]
    class Thread(c.Structure):
        _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('tid', w.DWORD), ('pid', w.DWORD),
                    ('base', w.LONG), ('delta', w.LONG), ('flags', w.DWORD)]
    k.Thread32First.argtypes = [w.HANDLE, c.POINTER(Thread)]
    k.Thread32Next.argtypes = [w.HANDLE, c.POINTER(Thread)]
    class Module(c.Structure):
        _fields_ = [('base', c.c_void_p), ('size', w.DWORD), ('entry', c.c_void_p)]
    k.K32EnumProcessModules.argtypes = [w.HANDLE, c.POINTER(w.HMODULE), w.DWORD, c.POINTER(w.DWORD)]
    k.K32GetModuleInformation.argtypes = [w.HANDLE, w.HMODULE, c.POINTER(Module), w.DWORD]
    k.K32GetModuleFileNameExW.argtypes = [w.HANDLE, w.HMODULE, w.LPWSTR, w.DWORD]
    k.GetThreadTimes.argtypes = [w.HANDLE, c.POINTER(w.FILETIME), c.POINTER(w.FILETIME),
                                c.POINTER(w.FILETIME), c.POINTER(w.FILETIME)]
    counts = Counter(); external_addresses = Counter(); errors = 0; module_map = []; thread = None
    with (args.out / 'child.stdout').open('wb') as out, (args.out / 'child.stderr').open('wb') as err:
        process = subprocess.Popen(command, cwd=args.out, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            snap = k.CreateToolhelp32Snapshot(4, 0)
            try:
                row = Thread(); row.size = c.sizeof(row); ok = k.Thread32First(snap, c.byref(row))
                candidates = []
                while ok:
                    if row.pid == process.pid:
                        h = k.OpenThread(0x4a, False, row.tid)
                        if h:
                            a, b, d, e = (w.FILETIME() for _ in range(4))
                            if k.GetThreadTimes(h, c.byref(a), c.byref(b), c.byref(d), c.byref(e)):
                                candidates.append(((a.dwHighDateTime << 32) | a.dwLowDateTime, row.tid, h))
                            else: k.CloseHandle(h)
                    ok = k.Thread32Next(snap, c.byref(row))
            finally: k.CloseHandle(snap)
            if not candidates: raise RuntimeError('Could not identify owned main thread')
            candidates.sort(); _, tid, thread = candidates[0]
            for _, _, h in candidates[1:]: k.CloseHandle(h)
            modules = (w.HMODULE * 1024)(); needed = w.DWORD()
            def refresh_modules():
                if not k.K32EnumProcessModules(int(process._handle), modules, c.sizeof(modules), c.byref(needed)):
                    return
                module_map.clear()
                for handle in modules[:needed.value // c.sizeof(w.HMODULE)]:
                    info = Module(); name = c.create_unicode_buffer(2048)
                    if k.K32GetModuleInformation(int(process._handle), handle, c.byref(info), c.sizeof(info)):
                        k.K32GetModuleFileNameExW(int(process._handle), handle, name, len(name))
                        module_map.append((info.base, info.base + info.size, Path(name.value).name, name.value))
            refresh_modules()
            last_module_refresh = time.monotonic()
            context_storage = c.create_string_buffer(1232 + 16)
            context_address = (c.addressof(context_storage) + 15) & ~15
            context = (c.c_char * 1232).from_address(context_address)
            deadline = time.monotonic() + 300
            while process.poll() is None:
                if time.monotonic() > deadline: raise TimeoutError('Owned finite workload exceeded 300 seconds')
                if not module_map: refresh_modules()
                struct.pack_into('<I', context, 48, 0x100001)
                suspended = k.SuspendThread(thread) != 0xffffffff
                try:
                    if suspended and k.GetThreadContext(thread, context_address):
                        pc = struct.unpack_from('<Q', context, 248)[0]
                        module = next((m for m in module_map if m[0] <= pc < m[1]), None)
                        # Popen can return before the loader has mapped SDL/CRT.
                        # Keep late DLLs attributable instead of freezing the startup map.
                        if module is None and time.monotonic()-last_module_refresh>1:
                            refresh_modules(); last_module_refresh=time.monotonic()
                            module=next((m for m in module_map if m[0]<=pc<m[1]),None)
                        name = 'unresolved-address'
                        if module and module[2].lower() == exe.name.lower():
                            offset = pc - module[0] + preferred
                            index = bisect.bisect_right(addresses, offset) - 1
                            name = entries[index][1] if index >= 0 else 'executable-header'
                        elif module:
                            name = 'DLL:' + module[2]
                            external_addresses[(module[3],pc-module[0])] += 1
                        counts[name] += 1
                    else: errors += 1
                finally:
                    if suspended: k.ResumeThread(thread)
                time.sleep(.003)
            if process.returncode: raise RuntimeError(f'Workload exited {process.returncode}')
        finally:
            if thread: k.CloseHandle(thread)
            if process.poll() is None:
                process.terminate(); process.wait(timeout=10)
    if identity != hashlib.sha256(exe.read_bytes()).hexdigest():
        raise RuntimeError('Executable changed during capture')
    report = {'scope': 'perturbed main-thread PC samples; not throughput', 'command': command,
              'exe_sha256': identity, 'symbols_sha256': hashlib.sha256(symbols.encode()).hexdigest(),
              'main_tid': tid, 'sample_errors': errors,
              'samples': sum(counts.values()), 'modules': module_map,
              'external_addresses': [{'module':n,'rva':hex(rva),'samples':value}
                                     for (n,rva),value in external_addresses.most_common()],
              'functions': [{'name': n, 'samples': value, 'percent': 100 * value / sum(counts.values())}
                            for n, value in counts.most_common()]}
    (args.out / 'profile.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k not in ('functions', 'modules')}, indent=2))


if __name__ == '__main__':
    main()

"""Fixed-count, serialized CPU A/A and balanced A/B software-SDL measurements.

No retry-until-pass. Both arms run identical enabled features and useful guest
work. Dummy video/audio avoid interrupting other playtests; GPU and physical
audio delivery are explicitly outside this CPU measurement.
"""
import argparse
import ctypes
import hashlib
import json
import msvcrt
import os
from pathlib import Path
import statistics
import struct
import subprocess
import time


def census():
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    def system_times():
        values = [ctypes.c_ulonglong() for _ in range(3)]
        if not kernel.GetSystemTimes(*(ctypes.byref(v) for v in values)):
            raise ctypes.WinError(ctypes.get_last_error())
        return [v.value for v in values]
    started = system_times()
    script = """
$sample=@{}; Get-Process MSBuild -ErrorAction SilentlyContinue | ForEach-Object { $sample[$_.Id]=$_.CPU }
Start-Sleep -Milliseconds 500
$processes=@(Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(cc1|cc1plus|cl|clang|ninja|msbuild|mingw32-make|.*Recomp|.*-runtime|n64lle)(\\.exe)?$' } | ForEach-Object {
    $delta=$null; $idle=$false
    if($_.Name -ieq 'MSBuild.exe' -and $_.CommandLine -match '/nodemode:1' -and $sample.ContainsKey([int]$_.ProcessId)) {
        $current=Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
        if($current) { $delta=$current.CPU-$sample[[int]$_.ProcessId]; $idle=$delta -lt 0.01 }
    }
    [pscustomobject]@{ProcessId=$_.ProcessId;Name=$_.Name;CommandLine=$_.CommandLine;IdleReusableNode=$idle;CpuDeltaSeconds=$delta}
})
$cpu=@(Get-CimInstance Win32_PerfFormattedData_PerfOS_Processor | Select-Object Name,PercentProcessorTime)
$disks=@(Get-CimInstance Win32_PerfFormattedData_PerfDisk_LogicalDisk | Where-Object {$_.Name -in @('F:','G:')} | Select-Object Name,CurrentDiskQueueLength,PercentDiskTime)
$memory=Get-CimInstance Win32_OperatingSystem
[pscustomobject]@{processes=$processes;cpu=$cpu;disks=$disks;free_memory_kib=$memory.FreePhysicalMemory} | ConvertTo-Json -Depth 4 -Compress
"""
    result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', script],
        capture_output=True, text=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode: raise RuntimeError('Workload census failed: ' + result.stderr)
    state = json.loads(result.stdout)
    delta = [b - a for a, b in zip(started, system_times())]
    state['cpu_percent_during_census'] = 100 * (1 - delta[0] / (delta[1] + delta[2]))
    # Idle reuse workers are recorded. An active build is not itself a veto:
    # the owner requested a decision from actual headroom, not its mere name.
    heavy = [r for r in state['processes'] if '--benchmark' in (r['CommandLine'] or '')
             or '--measure-runtime' in (r['CommandLine'] or '')]
    total = state['cpu_percent_during_census']
    if total > 60 or int(state['free_memory_kib']) < 4 * 1024 * 1024:
        heavy.append({'reason': 'Insufficient CPU or memory headroom', 'cpu_percent': total})
    if any(int(d['CurrentDiskQueueLength']) > (2 if d['Name'] == 'G:' else 8) for d in state['disks']):
        heavy.append({'reason': 'Storage queue exceeds the measurement budget', 'disks': state['disks']})
    return state, heavy


def choose_affinity(state):
    """Select two physical cores once, using OS topology and observed load."""
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    needed = ctypes.c_ulong()
    kernel.GetLogicalProcessorInformation(None, ctypes.byref(needed))
    data = ctypes.create_string_buffer(needed.value)
    if not kernel.GetLogicalProcessorInformation(data, ctypes.byref(needed)):
        raise ctypes.WinError(ctypes.get_last_error())
    if ctypes.sizeof(ctypes.c_void_p) != 8 or needed.value % 32:
        raise RuntimeError('Unexpected processor topology ABI')
    usage = {int(r['Name']): int(r['PercentProcessorTime']) for r in state['cpu'] if r['Name'].isdigit()}
    cores = []
    for off in range(0, needed.value, 32):
        mask, relationship = struct.unpack_from('<QI', data, off)
        if relationship == 0:
            bits = [i for i in range(64) if mask & (1 << i)]
            cores.append((sum(usage.get(i, 100) for i in bits) / len(bits), -mask, mask))
    if len(cores) < 2: raise RuntimeError('Two physical cores are required for this plan')
    cores.sort(); selected = cores[:2]
    return selected[0][2] | selected[1][2], cores


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--route', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    args = p.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    lock_path = Path(r'G:\Temp\Codex\benchmark-window.lock')
    report = {'scope': 'Windows process CPU; both eyes; SDL dummy video/audio; GPU/physical audio excluded',
              'plan': 'Per feature set: full-route warmup A,B; A,A noise control; A,B,B,A balanced pairs. Fixed two-core affinity. No retries within a series.',
              'headroom_policy': 'Start each run only with CPU <=60%, free RAM >=4GiB, queue G<=2/F<=8. Wait up to ten minutes for headroom, without repeating work. Record post-run load spikes; abort detected benchmark overlap. Assess CPU results using A/A noise and both pair directions.',
              'features': {'debug_tools': False, 'cpuhook': False, 'stereo': True, 'mods': False,
                           'audio_synthesis': True, 'audio_output': 'dummy', 'video_output': 'dummy'},
              'runs': [], 'summaries': [], 'passed': False,
              'identities': {name: {'path': str(path.resolve()), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                             for name, path in [('baseline', args.baseline), ('candidate', args.candidate),
                                                ('rom', args.rom), ('route', args.route)]}}
    useful_keys = ('frames', 'cycles', 'pc', 'wram_sha256', 'native_instructions',
                   'interpreted_instructions', 'fallback_instructions', 'presents', 'synthesized_audio_frames')
    def save(): (args.out / 'results.json').write_text(json.dumps(report, indent=2))
    with lock_path.open('a+b') as lock:
        if lock.tell() == 0: lock.write(b'0'); lock.flush()
        lock.seek(0)
        try: msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError: raise RuntimeError('Another benchmark owns ' + str(lock_path))
        try:
            deadline = time.monotonic() + 600
            while True:
                initial, blocked = census()
                if not blocked: break
                report.setdefault('deferred_starts', []).append({'label': 'initial', 'census': initial, 'reasons': blocked}); save()
                if any('CommandLine' in item for item in blocked) or time.monotonic() >= deadline:
                    report['blocked_by'] = blocked; raise RuntimeError('No measurement headroom')
                print('Waiting for initial CPU/storage headroom', flush=True); time.sleep(5)
            mask, cores = choose_affinity(initial)
            report['affinity_mask'] = hex(mask); report['topology_candidates'] = cores
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            expected = None
            for depth in (0, 32):
                for label in ('warm-A', 'warm-B', 'A1', 'A2', 'A3', 'B1', 'B2', 'A4'):
                    deadline = time.monotonic() + 600
                    while True:
                        before, heavy = census()
                        if not heavy: break
                        report.setdefault('deferred_starts', []).append({'label': label, 'depth': depth, 'census': before, 'reasons': heavy}); save()
                        if any('CommandLine' in item for item in heavy) or time.monotonic() >= deadline:
                            report['blocked_by'] = heavy; raise RuntimeError('Benchmark overlap or headroom deadline; work not repeated')
                        print('Waiting for CPU/storage headroom before ' + label, flush=True)
                        time.sleep(5)
                    arm = 'baseline' if label.endswith('A') or label.startswith('A') else 'candidate'
                    exe = (args.baseline if arm == 'baseline' else args.candidate).resolve()
                    directory = args.out / f'rewind-{depth}' / label; directory.mkdir(parents=True)
                    config = directory / 'settings.cfg'
                    config.write_text('fullscreen 0\naudio 1\nvolume 0\nscale 1\n')
                    command = [str(exe), '--rom', str(args.rom.resolve()), '--measure-runtime', '4000',
                               '--benchmark-route', str(args.route.resolve()), '--stereo', '--no-save',
                               '--config', str(config.resolve()), '--mods-dir', str((directory / 'mods').resolve())]
                    if depth: command += ['--rewind-frames', str(depth)]
                    environment = {k: v for k, v in os.environ.items() if not k.startswith('VBRECOMP_')}
                    environment.update(SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='dummy')
                    row = {'label': label, 'arm': arm, 'rewind_frames': depth, 'command': command,
                           'census_before': before, 'status': 'running'}
                    report['runs'].append(row); save()
                    started = time.monotonic()
                    with (directory / 'stdout.txt').open('wb') as out, (directory / 'stderr.txt').open('wb') as err:
                        child = subprocess.Popen(command, cwd=directory, env=environment, stdout=out, stderr=err,
                                                 creationflags=subprocess.CREATE_NO_WINDOW)
                        if not kernel.SetProcessAffinityMask(int(child._handle), mask):
                            child.terminate();child.wait(timeout=10);raise ctypes.WinError(ctypes.get_last_error())
                        try: code = child.wait(timeout=300)
                        except subprocess.TimeoutExpired:
                            child.terminate(); child.wait(timeout=10); row['status'] = 'timeout'; save(); raise
                    row['parent_wall_seconds'] = time.monotonic() - started
                    row['census_after'], heavy = census()
                    row['exit'] = code
                    lines = (directory / 'stdout.txt').read_text().splitlines()
                    payloads = [json.loads(line.split(' ', 1)[1]) for line in lines if line.startswith('VBRECOMP_BENCHMARK ')]
                    row['post_run_headroom_warnings'] = heavy
                    if code or len(payloads) != 1 or any('CommandLine' in item for item in heavy):
                        row['status'] = 'invalid'; save(); raise RuntimeError('Failed or externally contested measurement')
                    metric = payloads[0]; row['metric'] = metric
                    work = {k: metric[k] for k in useful_keys}
                    if expected is None: expected = work
                    if work != expected or metric['process_cpu_seconds'] <= 0:
                        row['status'] = 'invalid-work'; save(); raise RuntimeError('Useful work or CPU clock mismatch')
                    row['status'] = 'complete'; save()
                    print(f'rewind={depth} {label}: CPU {metric["process_cpu_seconds"]:.6f}s, {metric["fps"]:.3f} FPS', flush=True)
                rows = {r['label']: r['metric'] for r in report['runs'] if r['rewind_frames'] == depth}
                aa = abs(rows['A1']['process_cpu_seconds'] - rows['A2']['process_cpu_seconds']) / statistics.mean(
                    [rows['A1']['process_cpu_seconds'], rows['A2']['process_cpu_seconds']]) * 100
                a = statistics.mean([rows['A3']['process_cpu_seconds'], rows['A4']['process_cpu_seconds']])
                b = statistics.mean([rows['B1']['process_cpu_seconds'], rows['B2']['process_cpu_seconds']])
                pair_gains = [(rows['A3']['process_cpu_seconds'] - rows['B1']['process_cpu_seconds']) / rows['A3']['process_cpu_seconds'] * 100,
                              (rows['A4']['process_cpu_seconds'] - rows['B2']['process_cpu_seconds']) / rows['A4']['process_cpu_seconds'] * 100]
                summary = {'rewind_frames': depth, 'aa_cpu_noise_percent': aa, 'baseline_cpu_seconds': a,
                           'candidate_cpu_seconds': b, 'cpu_reduction_percent': (a - b) / a * 100,
                           'pairs_cpu_reduction_percent': pair_gains,
                           'exceeds_noise_in_both_directions': aa <= 3 and min(pair_gains) > 2 * aa,
                           'post_run_load_spikes': sum(bool(r.get('post_run_headroom_warnings')) for r in report['runs'] if r['rewind_frames'] == depth)}
                report['summaries'].append(summary); save()
            report['passed'] = True; save(); print(json.dumps(report['summaries'], indent=2))
        except Exception as error:
            report['error'] = str(error); save(); raise
        finally:
            lock.seek(0); msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)


if __name__ == '__main__':
    main()

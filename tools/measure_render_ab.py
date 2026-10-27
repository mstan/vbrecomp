"""Matched finite production rendering measurements; owns the shared host lock.

No game saves are used. Dummy SDL still executes software composition, texture
upload/presentation and audio synthesis; physical GPU/audio delivery is excluded.
Exit 75 means defer, without launching a workload. The caller can try another
host or check again after the campaign's 30-minute interval.
"""
import argparse
import hashlib
import json
import msvcrt
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
from measure_ab_windows import census, choose_affinity
from prepare_benchmark_route import convert_route


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'rom', 'route', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--color', type=Path)
    p.add_argument('--wide', type=Path)
    p.add_argument('--aspect', default='adaptive')
    p.add_argument('--window', default='522x448', help='Unscaled stereo 21:9 pixel grid, avoiding unrelated software-upscale cost')
    p.add_argument('--package', required=True)
    p.add_argument('--history', type=int, choices=(0, 32), default=32)
    p.add_argument('--profile', action='store_true')
    p.add_argument('--nm', type=Path, default=Path('C:/msys64/mingw64/bin/nm.exe'))
    p.add_argument('--symbols', type=Path, help='Matching baseline symbols for profiling on a second host')
    args = p.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    text, frames = convert_route(json.loads(args.route.read_text(encoding='utf-8-sig')))
    route = args.out / 'route.txt'
    route.write_text(text, encoding='ascii')
    report = dict(passed=False, package=args.package, host=platform.node(), platform=platform.platform(),
                  scope='Windows process CPU; stereo; SDL dummy video/audio; physical GPU/audio excluded',
                  features=dict(color=bool(args.color), widescreen=args.aspect if args.wide else 'off', window=args.window,
                                rewind_depth=args.history, rewind_interval=6, debug_tools=False, cpuhook=False), frames=frames, runs=[])
    report['identities'] = {n: dict(path=str(v.resolve()), sha256=hashlib.sha256(v.read_bytes()).hexdigest())
        for n in ('baseline', 'candidate', 'rom', 'route', 'color', 'wide') if (v := getattr(args, n))}
    def save():
        (args.out / 'results.json').write_text(json.dumps(report, indent=2))
    save()
    lock_path = Path('G:/Temp/Codex/benchmark-window.lock')
    with lock_path.open('a+b') as lock:
        if lock.tell() == 0:
            lock.write(b'0'); lock.flush()
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            report['deferred'] = 'Another campaign benchmark owns the shared host lock'; save()
            return 75
        try:
            state, busy = census()
            report['census_initial'] = state
            # Existing cross-engine scripts do not all acquire this older lock.
            query = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                "Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^python' -and $_.CommandLine -match 'measure_production.py|measure_native|measure_ab_windows.py|profile_native_windows.py' } | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress"],
                capture_output=True, text=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
            report['other_measurement_scripts'] = query.stdout.strip()
            if busy or query.stdout.strip():
                report['deferred'] = busy or report['other_measurement_scripts']; save()
                return 75
            mask, cores = choose_affinity(state)
            report.update(affinity_mask=hex(mask), topology_candidates=cores)
            import ctypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            env = {k: v for k, v in os.environ.items() if not k.startswith('VBRECOMP_')}
            env.update(SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER='dummy')
            expected = None
            labels = ('profile',) if args.profile else ('warm-A', 'warm-B', 'A1', 'A2', 'A3', 'B1', 'B2', 'A4', 'A5', 'B3')
            for label in labels:
                before, busy = census()
                if busy:
                    report['deferred'] = busy; save(); return 75
                arm = 'candidate' if label.startswith('B') or label == 'warm-B' else 'baseline'
                directory = args.out / label
                directory.mkdir()
                config = directory / 'settings.cfg'
                config.write_text('fullscreen 0\naudio 1\nvolume 0\nscale 1\n')
                command = [str(getattr(args, arm).resolve()), '--rom', str(args.rom.resolve()),
                    '--measure-runtime', str(frames), '--measure-window', args.window,
                    '--benchmark-route', str(route), '--stereo', '--no-save',
                    '--config', str(config), '--mods-dir', str(directory / 'mods')]
                if args.history:
                    command += ['--rewind-frames', str(args.history), '--rewind-interval', '6']
                for archive, feature in ((args.color, 'wario-land-color:color'), (args.wide, 'wario-land-widescreen:widescreen')):
                    if archive:
                        command += ['--install-mod', str(archive.resolve()), '--enable-mod', feature]
                if args.wide:
                    command += ['--set-mod-option', 'wario-land-widescreen:widescreen:aspect=' + args.aspect]
                if args.profile:
                    command = [sys.executable, str(Path(__file__).with_name('profile_native_windows.py')),
                               '--out', str(directory / 'capture'),
                               '--symbols' if args.symbols else '--nm',
                               str(args.symbols.resolve()) if args.symbols else str(args.nm), '--', *command]
                row = dict(label=label, arm=arm, command=command, census_before=before, status='running')
                report['runs'].append(row); save()
                launched = time.perf_counter()
                with (directory / 'stdout.txt').open('wb') as out, (directory / 'stderr.txt').open('wb') as err:
                    child = subprocess.Popen(command, cwd=directory, env=env, stdout=out, stderr=err,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
                    if not args.profile and not kernel.SetProcessAffinityMask(int(child._handle), mask):
                        child.terminate(); child.wait(); raise ctypes.WinError(ctypes.get_last_error())
                    try:
                        code = child.wait(timeout=300)
                    except subprocess.TimeoutExpired:
                        child.terminate(); child.wait(); raise
                row['exit_code'] = code
                row['launch_to_exit_seconds'] = time.perf_counter() - launched
                if code:
                    raise RuntimeError('Finite child failed: ' + label)
                if args.profile:
                    row['status'] = 'profiled'; report['passed'] = True; save(); return 0
                payload = [json.loads(s.split(' ', 1)[1]) for s in (directory / 'stdout.txt').read_text().splitlines()
                           if s.startswith('VBRECOMP_BENCHMARK ')]
                if len(payload) != 1:
                    raise RuntimeError('Missing or ambiguous measurement')
                metric = payload[0]
                work = {k: metric[k] for k in ('frames', 'cycles', 'pc', 'wram_sha256', 'native_instructions',
                    'interpreted_instructions', 'fallback_instructions', 'presents', 'synthesized_audio_frames')}
                if expected is None:
                    expected = work
                if work != expected or metric['process_cpu_seconds'] <= 0:
                    raise RuntimeError('Unequal useful work: ' + label)
                row.update(metric=metric, status='complete')
                row['census_after'], row['headroom_warnings'] = census()
                save()
                print(f'{label}: {metric["process_cpu_seconds"]:.6f}s CPU, {metric["fps"]:.3f} fps', flush=True)
            runs = {r['label']: r['metric'] for r in report['runs']}
            noise = abs(runs['A1']['process_cpu_seconds'] - runs['A2']['process_cpu_seconds']) / statistics.mean(
                [runs['A1']['process_cpu_seconds'], runs['A2']['process_cpu_seconds']]) * 100
            pairs = [(a, b) for a, b in (('A3', 'B1'), ('A4', 'B2'), ('A5', 'B3'))]
            savings = [100 * (1 - runs[b]['process_cpu_seconds'] / runs[a]['process_cpu_seconds']) for a, b in pairs]
            report['summary'] = dict(aa_cpu_noise_percent=noise, pairs_cpu_savings_percent=savings,
                median_cpu_savings_percent=statistics.median(savings),
                pairs_throughput_gain_percent=[100 * (runs[b]['fps'] / runs[a]['fps'] - 1) for a, b in pairs])
            report['passed'] = True; save()
            print(json.dumps(report['summary'], indent=2))
        except Exception as error:
            report['error'] = str(error); save(); raise
        finally:
            lock.seek(0); msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

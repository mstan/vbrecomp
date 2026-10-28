"""Matched finite production rendering measurements; owns the shared host lock.

No game saves are used. Dummy SDL still executes software composition, texture
upload/presentation and audio synthesis; physical GPU/audio delivery is excluded.
Exit 75 means defer, without launching a workload. The caller can try another
host or check again after the campaign's 30-minute interval.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
from measure_ab_windows import census, choose_affinity
from prepare_benchmark_route import convert_route
from performance_window_windows import PerformanceWindow,PerformanceWindowBusy

WORK_KEYS=('frames','cycles','pc','wram_sha256','native_instructions',
           'interpreted_instructions','fallback_instructions','presents','synthesized_audio_frames')
DEFERRED_PREFIX=('warm-A','warm-B','A1','A2','A3','B1','B2','A4','A5')

def validate_deferred_completion(previous,current):
    """Only finish a never-launched third candidate; never repeat an outcome."""
    if previous.get('passed') or not previous.get('deferred') or previous.get('error'):
        raise ValueError('Completion requires a resource-deferred original series')
    rows=previous.get('runs',[])
    if tuple(r.get('label') for r in rows)!=DEFERRED_PREFIX:
        raise ValueError('Only the complete prefix ending before B3 can be continued')
    if any(r.get('status')!='complete' or r.get('headroom_warnings') or 'metric'not in r for r in rows):
        raise ValueError('All retained samples must be complete with clear headroom')
    for key in ('package','host','platform','scope','features','frames','identities','dependency_sha256'):
        if previous.get(key)!=current.get(key):raise ValueError('Changed completion identity: '+key)
    expected={k:rows[0]['metric'][k] for k in WORK_KEYS}
    if any({k:r['metric'][k] for k in WORK_KEYS}!=expected for r in rows):
        raise ValueError('Retained useful work differs')
    mask=int(previous['affinity_mask'],16)
    if not mask:raise ValueError('Missing original affinity')
    return mask,expected


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'rom', 'route', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--color', type=Path)
    p.add_argument('--wide', type=Path)
    p.add_argument('--color-feature', default='wario-land-color:color')
    p.add_argument('--game-argument', action='append', default=[], help='Additional identical game argument; use = for a leading dash')
    p.add_argument('--aspect', default='adaptive')
    p.add_argument('--window', default='522x448', help='Unscaled stereo 21:9 pixel grid, avoiding unrelated software-upscale cost')
    p.add_argument('--package', required=True)
    p.add_argument('--history', type=int, choices=(0, 32), default=32)
    p.add_argument('--profile', action='store_true')
    p.add_argument('--profile-first', action='store_true', help='Retain a diagnostic profile before the fixed uninstrumented A/B series')
    p.add_argument('--profile-arm', choices=('baseline','candidate'), default='baseline')
    p.add_argument('--video-driver', choices=('dummy','windows'), default='dummy', help='Windows is a muted, nonactivating physical-presentation diagnostic')
    p.add_argument('--nm', type=Path, default=Path('C:/msys64/mingw64/bin/nm.exe'))
    p.add_argument('--symbols', type=Path, help='Matching baseline symbols for profiling on a second host')
    p.add_argument('--stackwalker', type=Path, help='Optional caller-stack profiler helper; never used for A/B timing')
    p.add_argument('--complete-deferred',type=Path,help='Original results.json stopped before B3; retain it and finish A6,A7,B3 in a new output directory')
    args = p.parse_args()
    if args.profile and args.profile_first:p.error('Choose profile-only or profile-first')
    if args.complete_deferred and (args.profile or args.profile_first or args.video_driver!='dummy'):
        p.error('Deferred completion is limited to uninstrumented dummy-video timing')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    text, frames = convert_route(json.loads(args.route.read_text(encoding='utf-8-sig')))
    route = args.out / 'route.txt'
    route.write_text(text, encoding='ascii')
    report = dict(passed=False, package=args.package, host=platform.node(), platform=platform.platform(),
                  scope='Windows process CPU; stereo; SDL dummy video/audio; physical GPU/audio excluded',
                  features=dict(color=bool(args.color), widescreen=args.aspect if args.wide else 'off', window=args.window,
                                rewind_depth=args.history, rewind_interval=6, debug_tools=False, cpuhook=False), frames=frames, runs=[])
    report['features'].update(color_feature=args.color_feature if args.color else None,game_arguments=args.game_argument)
    report['features']['video_driver']=args.video_driver
    if args.video_driver=='windows':report['scope']='Physical Windows presentation diagnostic; muted audio synthesis; ordinary background window; wall profiles include GPU waits'
    report.update(command=sys.argv,harness_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  profile_arm=args.profile_arm,
                  dependency_sha256={name:hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                    for name in ('measure_ab_windows.py','prepare_benchmark_route.py','profile_native_windows.py','performance_window_windows.py')})
    report['identities'] = {n: dict(path=str(v.resolve()), sha256=hashlib.sha256(v.read_bytes()).hexdigest())
        for n in ('baseline', 'candidate', 'rom', 'route', 'color', 'wide') if (v := getattr(args, n))}
    previous=None
    if args.complete_deferred:
        previous=json.loads(args.complete_deferred.read_text())
        validate_deferred_completion(previous,report)
        report['runs']=copy.deepcopy(previous['runs'])
        report['completion']=dict(original=str(args.complete_deferred.resolve()),
            original_sha256=hashlib.sha256(args.complete_deferred.read_bytes()).hexdigest(),
            original_harness_sha256=previous['harness_sha256'],
            plan='Retain original samples; A5 remains unpaired across the resource gap. Fresh A6,A7 noise control, then never-launched B3. Pairs A3/B1, A4/B2, A7/B3. No candidate outcome is repeated.')
    def save():
        (args.out / 'results.json').write_text(json.dumps(report, indent=2))
    save()
    with PerformanceWindow() as lock:
        report['lock_paths']=[str(p) for p in lock.paths]
        try:
            lock.acquire()
        except PerformanceWindowBusy as error:
            report['deferred'] = str(error); save()
            return 75
        try:
            state, busy = census()
            report['census_initial'] = state
            # Existing cross-engine scripts do not all acquire this older lock.
            query = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command',
                "Get-CimInstance Win32_Process | Where-Object { $_.ProcessId -ne " + str(os.getpid()) +
                " -and $_.Name -match '^python' -and $_.CommandLine -match 'measure_[\\w-]+[.]py|benchmark[\\w-]*[.]py|profile_native_windows[.]py|run_pair[.]py|run_ab[\\w-]*[.]py|stress[\\w-]*[.]py' } | Select-Object ProcessId,CommandLine | ConvertTo-Json -Compress"],
                capture_output=True, text=True, check=True, creationflags=subprocess.CREATE_NO_WINDOW)
            report['other_measurement_scripts'] = query.stdout.strip()
            if busy or query.stdout.strip():
                report['deferred'] = busy or report['other_measurement_scripts']; save()
                return 75
            if previous:
                mask,_=validate_deferred_completion(previous,report)
                cores=previous['topology_candidates']
            else:mask, cores = choose_affinity(state)
            report.update(affinity_mask=hex(mask), topology_candidates=cores)
            import ctypes
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
            class MemoryCounters(ctypes.Structure):
                _fields_=[('cb',ctypes.c_ulong),('faults',ctypes.c_ulong)]+[
                    (name,ctypes.c_size_t) for name in ('peak_ws','ws','peak_paged_pool','paged_pool',
                        'peak_nonpaged_pool','nonpaged_pool','pagefile','peak_pagefile','private')]
            kernel.K32GetProcessMemoryInfo.argtypes=[ctypes.c_void_p,ctypes.POINTER(MemoryCounters),ctypes.c_ulong]
            kernel.K32GetProcessMemoryInfo.restype=ctypes.c_int
            env = {k: v for k, v in os.environ.items() if not k.startswith('VBRECOMP_')}
            env.update(SDL_AUDIODRIVER='dummy', SDL_VIDEODRIVER=args.video_driver,
                       SDL_WINDOW_NO_ACTIVATION_WHEN_SHOWN='1')
            user=ctypes.WinDLL('user32',use_last_error=True)
            user.GetForegroundWindow.restype=ctypes.c_void_p
            user.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
            def foreground_pid():
                pid=ctypes.c_ulong();user.GetWindowThreadProcessId(user.GetForegroundWindow(),ctypes.byref(pid));return pid.value
            def record_backend(metric,row,pid):
                row['presentation_backend']={key:metric.get(key) for key in
                    ('sdl_video_active','sdl_video_dummy','sdl_renderer_flags','sdl_renderer_opengl')}
                if args.video_driver=='windows':
                    row['owned_runtime_pid']=pid
                    if not isinstance(pid,int) or pid<=0:
                        raise RuntimeError('Physical diagnostic requires an identified owned runtime process')
                    if metric.get('sdl_video_active')!=1 or metric.get('sdl_video_dummy')!=0:
                        raise RuntimeError('Physical diagnostic did not establish a real video driver')
                    if pid in row['foreground_pids_observed']:
                        raise RuntimeError('Owned runtime was observed in the foreground; stop unattended physical launches')
            expected = None
            labels = ('warm-A', 'warm-B', 'A1', 'A2', 'A3', 'B1', 'B2', 'A4', 'A5', 'B3')
            if previous:
                _,expected=validate_deferred_completion(previous,report)
                labels=('A6','A7','B3')
            if args.profile:labels=('profile',)
            elif args.profile_first:labels=('profile',*labels)
            for label in labels:
                profiling=label=='profile'
                before, busy = census()
                if busy:
                    report['deferred'] = busy; save(); return 75
                arm = 'candidate' if label.startswith('B') or label == 'warm-B' else 'baseline'
                if profiling:arm=args.profile_arm
                directory = args.out / label
                directory.mkdir()
                config = directory / 'settings.cfg'
                config.write_text('fullscreen 0\naudio 1\nvolume 0\nscale 1\n')
                command = [str(getattr(args, arm).resolve()), '--rom', str(args.rom.resolve()),
                    '--measure-runtime', str(frames), '--measure-window', args.window,
                    '--benchmark-route', str(route), '--stereo', '--no-save',
                    '--config', str(config), '--mods-dir', str(directory / 'mods')]
                command += args.game_argument
                if args.history:
                    command += ['--rewind-frames', str(args.history), '--rewind-interval', '6']
                for archive, feature in ((args.color, args.color_feature), (args.wide, 'wario-land-widescreen:widescreen')):
                    if archive:
                        command += ['--install-mod', str(archive.resolve()), '--enable-mod', feature]
                if args.wide:
                    command += ['--set-mod-option', 'wario-land-widescreen:widescreen:aspect=' + args.aspect]
                if profiling:
                    command = [sys.executable, str(Path(__file__).with_name('profile_native_windows.py')),
                               '--out', str(directory / 'capture'),
                               '--symbols' if args.symbols else '--nm',
                               str(args.symbols.resolve()) if args.symbols else str(args.nm),
                               *(['--stackwalker', str(args.stackwalker.resolve())] if args.stackwalker else []),
                               '--', *command]
                row = dict(label=label, arm=arm, command=command, census_before=before, status='running')
                report['runs'].append(row); save()
                launched = time.perf_counter()
                foreground_ids=set()
                if args.video_driver=='windows':row['foreground_before_pid']=foreground_pid()
                with (directory / 'stdout.txt').open('wb') as out, (directory / 'stderr.txt').open('wb') as err:
                    child = subprocess.Popen(command, cwd=directory, env=env, stdout=out, stderr=err,
                                             creationflags=subprocess.CREATE_NO_WINDOW)
                    if not profiling and not kernel.SetProcessAffinityMask(int(child._handle), mask):
                        child.terminate(); child.wait(); raise ctypes.WinError(ctypes.get_last_error())
                    try:
                        deadline=time.perf_counter()+(330 if profiling else 300)
                        memory=dict(scope='Owned runtime process; OS peak working set/pagefile fields and 100ms sampled PrivateUsage',
                                    samples=0,peak_working_set_bytes=0,peak_pagefile_usage_bytes=0,sampled_peak_private_usage_bytes=0)
                        while True:
                            if args.video_driver=='windows':foreground_ids.add(foreground_pid())
                            if not profiling:
                                counters=MemoryCounters();counters.cb=ctypes.sizeof(counters)
                                if kernel.K32GetProcessMemoryInfo(int(child._handle),ctypes.byref(counters),counters.cb):
                                    memory['samples']+=1
                                    memory['peak_working_set_bytes']=max(memory['peak_working_set_bytes'],counters.peak_ws)
                                    memory['peak_pagefile_usage_bytes']=max(memory['peak_pagefile_usage_bytes'],counters.peak_pagefile)
                                    memory['sampled_peak_private_usage_bytes']=max(memory['sampled_peak_private_usage_bytes'],counters.private)
                            remaining=deadline-time.perf_counter()
                            if remaining<=0:raise subprocess.TimeoutExpired(command,330 if profiling else 300)
                            try:
                                code=child.wait(timeout=min(.1,remaining));break
                            except subprocess.TimeoutExpired:pass
                        if not profiling:row['process_memory']=memory
                    except subprocess.TimeoutExpired:
                        child.terminate(); child.wait(); raise
                row['exit_code'] = code
                if args.video_driver=='windows':row['foreground_pids_observed']=sorted(foreground_ids)
                row['launch_to_exit_seconds'] = time.perf_counter() - launched
                if code:
                    row['status']='failed';save()
                    raise RuntimeError('Finite child failed: ' + label)
                if profiling:
                    payload=[json.loads(s.split(' ',1)[1]) for s in (directory/'capture'/'child.stdout').read_text().splitlines()
                             if s.startswith('VBRECOMP_BENCHMARK ')]
                    if len(payload)!=1 or payload[0].get('frames')!=frames:
                        row.update(status='invalid_profile',measurement_count=len(payload));save()
                        raise RuntimeError('Profile did not establish the complete finite workload')
                    row['profile_work']={k:payload[0][k] for k in ('frames','cycles','pc','wram_sha256','native_instructions',
                        'interpreted_instructions','fallback_instructions','presents','synthesized_audio_frames')}
                    actual_profile=json.loads((directory/'capture'/'profile.json').read_text())
                    record_backend(payload[0],row,actual_profile.get('process_id'))
                    row['status']='profiled';save()
                    if args.profile:report['passed']=True;save();return 0
                    continue
                payload = [json.loads(s.split(' ', 1)[1]) for s in (directory / 'stdout.txt').read_text().splitlines()
                           if s.startswith('VBRECOMP_BENCHMARK ')]
                if len(payload) != 1:
                    row.update(status='invalid_result',measurement_count=len(payload));save()
                    raise RuntimeError('Missing or ambiguous measurement')
                metric = payload[0]
                record_backend(metric,row,child.pid)
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
            runs = {r['label']: r['metric'] for r in report['runs'] if 'metric' in r}
            noise = abs(runs['A1']['process_cpu_seconds'] - runs['A2']['process_cpu_seconds']) / statistics.mean(
                [runs['A1']['process_cpu_seconds'], runs['A2']['process_cpu_seconds']]) * 100
            pairs = [(a, b) for a, b in (('A3', 'B1'), ('A4', 'B2'), ('A5', 'B3'))]
            if previous:
                completion_noise=abs(runs['A6']['process_cpu_seconds']-runs['A7']['process_cpu_seconds'])/statistics.mean(
                    [runs['A6']['process_cpu_seconds'],runs['A7']['process_cpu_seconds']])*100
                report['completion'].update(original_aa_percent=noise,completion_aa_percent=completion_noise,
                    baseline_drift_percent=100*(runs['A6']['process_cpu_seconds']/runs['A5']['process_cpu_seconds']-1))
                noise=max(noise,completion_noise)
                pairs=[('A3','B1'),('A4','B2'),('A7','B3')]
            savings = [100 * (1 - runs[b]['process_cpu_seconds'] / runs[a]['process_cpu_seconds']) for a, b in pairs]
            report['summary'] = dict(aa_cpu_noise_percent=noise, pairs_cpu_savings_percent=savings,
                pair_labels=pairs,
                median_cpu_savings_percent=statistics.median(savings),
                pairs_throughput_gain_percent=[100 * (runs[b]['fps'] / runs[a]['fps'] - 1) for a, b in pairs])
            report['passed'] = True; save()
            print(json.dumps(report['summary'], indent=2))
        except Exception as error:
            report['error'] = str(error); save(); raise
        finally:
            lock.release()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())

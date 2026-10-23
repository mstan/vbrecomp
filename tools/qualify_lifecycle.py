"""Byte-audited current-build restore, rewind and replay qualification.

Uses an owned headless runtime and isolated files. No native save is read or
written. This proves local lifecycle behavior; it makes no netplay claim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib
from cosim import Runner, compare_snapshots
from validate_debugger import wait_stop

PLANES = ['cpu', 'wram', 'sram', 'vram', 'vip', 'devices', 'video', 'video_right', 'presented']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--port', type=int, default=15870)
    parser.add_argument('--route', type=Path)
    parser.add_argument('--checkpoint', type=int, default=2600)
    parser.add_argument('--sources', action='store_true', help='Require per-eye native/presented provenance dumps')
    parser.add_argument('--mod-archive', type=Path, help='Install into the isolated test profile')
    parser.add_argument('--enable-mod', help='Isolated package:feature to enable')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'checks': [], 'scope': 'local current-build lifecycle; netplay deferred',
              'runtime_sha256': hashlib.sha256(args.runtime.read_bytes()).hexdigest(),
              'rom_sha256': hashlib.sha256(args.rom.read_bytes()).hexdigest()}

    def check(name, condition):
        report['checks'].append({'name': name, 'passed': bool(condition)})
        if not condition:
            raise AssertionError(name)

    try:
        extra = []
        if args.mod_archive: extra += ['--install-mod', str(args.mod_archive.resolve())]
        if args.enable_mod: extra += ['--enable-mod', args.enable_mod]
        report['mod'] = {'archive': str(args.mod_archive), 'enabled': args.enable_mod}
        with Runner(args.runtime, args.rom, 'hybrid', args.port, args.out / 'runtime', extra=extra) as r:
            def snapshot(frame, planes):
                data = r.snapshot(frame, planes)
                if args.sources:
                    for eye in (0, 1):
                        for presented in (0, 1):
                            key = f'sources_{eye}_{presented}'
                            path = r.directory / f'frame-{frame}-{key}.bin'
                            r.client.command('source_dump', path=path.as_posix(), eye=eye, presented=presented)
                            data[key] = path.read_bytes()
                return data
            initial = (args.out / 'initial.state').resolve()
            partial = (args.out / 'partial.state').resolve()
            r.client.command('save_state', path=initial.as_posix())
            r.client.command('step', count=17); wait_stop(r.client)
            registers = r.client.command('get_registers')
            check('instruction step reaches a mid-frame dispatch boundary',
                  registers['frame'] == 0 and 0 < registers['cycles'] < 397824)
            r.client.command('save_state', path=partial.as_posix())
            r.client.run_frames(3)
            partial_end = snapshot(3, PLANES + ['audio'])
            r.client.command('load_state', path=partial.as_posix())
            r.client.run_frames(3)
            check('mid-frame restore preserves dispatch/device timing and stereo PCM',
                  not compare_snapshots(partial_end, snapshot(3, PLANES + ['audio'])))
            r.client.command('load_state', path=initial.as_posix())
            if args.route:
                prefix, tail, cursor = [], [], 0
                for segment in json.loads(args.route.read_text()):
                    count, pad = segment['frames'], segment.get('pad', 0)
                    left = min(count, max(0, args.checkpoint - cursor))
                    if left: prefix.append((left, pad))
                    if count > left: tail.append((count - left, pad))
                    cursor += count
                base, finish = sum(n for n, _ in prefix), cursor
                if not tail or base != args.checkpoint:
                    raise ValueError('checkpoint must be inside the existing route')
            else:
                prefix, tail = [(300, 0)], [(10, 4096), (90, 0), (10, 4096), (90, 0)]
                base, finish = 300, 500
            def run(schedule):
                for frames, pad in schedule:
                    r.client.command('set_input', pad=pad); r.client.run_frames(frames)
            run(prefix)
            before = snapshot(base, PLANES)
            digest = r.client.command('state_digest')['digest']
            state = (args.out / 'checkpoint.state').resolve()
            r.client.command('save_state', path=state.as_posix())
            check('snapshot file is nonempty', state.stat().st_size > 112)
            # Present repeatedly with both eyes, checking actual decoded pixels
            # and canonical state rather than using a matching hash alone.
            for repeat in range(3):
                shown = snapshot(base, ['video', 'video_right', 'presented'])
                check(f'repeated presentation {repeat}', all(shown[p] == before[p] for p in shown))
            check('presentation is read-only', r.client.command('state_digest')['digest'] == digest)
            for addr in (0x05001234, 0x06002345, 0x00006040,
                         0x00000020, 0x00008020, 0x00010020, 0x00018020):
                value = r.memory(addr, 1)[0]
                r.client.command('write_ram', addr=addr, val=value ^ 255)
                check(f'direct write {addr:08x} exercised', r.memory(addr, 1)[0] == value ^ 255)
            r.client.command('load_state', path=state.as_posix())
            check('direct writes restored across all planes', not compare_snapshots(before, snapshot(base, PLANES)))
            check('restore canonical digest', r.client.command('state_digest')['digest'] == digest)

            original = state.read_bytes()
            # A failed temporary-file write must preserve the previous file.
            # A directory is a portable, deterministic refusal of fopen("wb").
            temporary = Path(str(state) + '.tmp')
            temporary.mkdir()
            try:
                try:
                    r.client.command('save_state', path=state.as_posix())
                except RuntimeError:
                    rejected = True
                else:
                    rejected = False
                check('failed atomic save preserves checkpoint', rejected and state.read_bytes() == original)
            finally:
                temporary.rmdir()
            damaged = []
            damaged.append(('truncated', original[:100]))
            blob = bytearray(original); blob[-1] ^= 1; damaged.append(('bad_crc', blob))
            blob = bytearray(original); blob[16] ^= 1; damaged.append(('other_build', blob))
            blob = bytearray(original); blob[48] ^= 1; damaged.append(('other_rom', blob))
            blob = bytearray(original); blob[88] ^= 1; damaged.append(('other_presentation_features', blob))
            blob = bytearray(original); struct.pack_into('<I', blob, 12, 3)
            damaged.append(('private_memory_flags_rejected_as_file', blob))
            # Valid CRC but illegal hardwired r0 exercises the transactional
            # validator after decoding, not only the outer checksum rejection.
            blob = bytearray(original); blob[112] = 1
            struct.pack_into('<I', blob, 80, zlib.crc32(blob[112:]))
            damaged.append(('invalid_cpu_with_valid_crc', blob))
            for name, blob in damaged:
                path = args.out / (name + '.state'); path.write_bytes(blob)
                try:
                    r.client.command('load_state', path=path.resolve().as_posix())
                except RuntimeError:
                    rejected = True
                else:
                    rejected = False
                check(name + ' rejected atomically', rejected and r.client.command('state_digest')['digest'] == digest)

            def advance():
                run(tail)
            advance()
            end = snapshot(finish, PLANES + ['audio'])
            end_digest = r.client.command('state_digest')['digest']
            r.client.command('load_state', path=state.as_posix())
            advance()
            check('restore and deterministic replay including stereo PCM',
                  not compare_snapshots(end, snapshot(finish, PLANES + ['audio'])))
            check('replayed canonical digest', end_digest == r.client.command('state_digest')['digest'])
            r.client.command('load_state', path=state.as_posix())
            for frames, pad in tail:
                r.client.command('set_input', pad=pad)
                r.client.command('replay_frames', frames=frames)
            check('direct frame-replay helper matches main execution and stereo PCM',
                  not compare_snapshots(end, snapshot(finish, PLANES + ['audio'])))

            r.client.command('rewind_enable', value=8)
            r.client.run_frames(3)
            end = snapshot(finish + 3, PLANES)
            r.client.command('rewind', value=finish + 1)
            check('rewind restores requested frame', r.client.command('frame')['frame'] == finish + 1)
            r.client.run_frames(2)
            check('rewind branches replay exactly', not compare_snapshots(end, snapshot(finish + 3, PLANES)))
            r.client.command('write_ram', addr=0x06002345, val=91)
            r.client.command('reset')
            check('reset retains native SRAM', r.memory(0x06002345, 1) == bytes([91]))
            check('reset reaches frame zero', r.client.command('frame')['frame'] == 0)
            r.client.command('load_state', path=state.as_posix())
            check('restore survives reset', not compare_snapshots(before, snapshot(base, PLANES)))
            check('no dispatch miss log', not any(p.read_text().strip() for p in r.directory.glob('dispatch_misses.log')))
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        (args.out / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

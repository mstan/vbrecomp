"""Check real same-source, different-configuration snapshot rejection."""
import argparse
import hashlib
import json
from pathlib import Path
from cosim import Runner


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runtime-a', type=Path, required=True)
    p.add_argument('--runtime-b', type=Path, required=True)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--port', type=int, default=15872)
    args = p.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'checks': [], 'binaries': {
        k: hashlib.sha256(v.read_bytes()).hexdigest()
        for k, v in [('a', args.runtime_a), ('b', args.runtime_b)]}}
    def check(name, value):
        report['checks'].append({'name': name, 'passed': bool(value)})
        if not value: raise AssertionError(name)
    try:
        files = [(args.out / (name + '.state')).resolve() for name in ('a', 'b')]
        for exe, path in zip((args.runtime_a, args.runtime_b), files):
            with Runner(exe, args.rom, 'hybrid', args.port, path.parent / path.stem) as r:
                r.client.command('save_state', path=path.as_posix())
        a, b = [path.read_bytes() for path in files]
        check('same snapshot layout, cartridge and presentation features',
              len(a) == len(b) and a[48:80] == b[48:80] and a[88:112] == b[88:112])
        check('actual build configurations have distinct identities', a[16:48] != b[16:48])
        for name, exe, own, other in [('a', args.runtime_a, files[0], files[1]),
                                      ('b', args.runtime_b, files[1], files[0])]:
            with Runner(exe, args.rom, 'hybrid', args.port, args.out / ('reload-' + name)) as r:
                digest = r.client.command('state_digest')['digest']
                try: r.client.command('load_state', path=other.as_posix())
                except RuntimeError: rejected = True
                else: rejected = False
                check(name + ' rejects other configuration atomically',
                      rejected and r.client.command('state_digest')['digest'] == digest)
                r.client.command('load_state', path=own.as_posix())
                check(name + ' accepts its own current-build snapshot',
                      r.client.command('state_digest')['digest'] == digest)
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    (args.out / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

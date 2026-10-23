"""Protect isolated native SRAM across execution-state restoration and bad input."""
import argparse
import hashlib
import json
from pathlib import Path
from cosim import Runner


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runtime', type=Path, required=True)
    p.add_argument('--rom', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--port', type=int, default=15870)
    args = p.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    report = {'passed': False, 'checks': [], 'runtime_sha256': hashlib.sha256(args.runtime.read_bytes()).hexdigest()}
    def check(name, value):
        report['checks'].append({'name': name, 'passed': bool(value)})
        if not value: raise AssertionError(name)
    try:
        native = (args.out / 'cartridge.sav').resolve()
        checkpoint = (args.out / 'old-execution.state').resolve()
        original = bytes((i * 13 + 7) & 255 for i in range(65536))
        native.write_bytes(original)
        extra = ['--save', str(native)]
        with Runner(args.runtime, args.rom, 'hybrid', args.port, args.out / 'first', extra) as r:
            check('existing native save loads exactly', r.memory(0x06000000) == original)
            r.client.command('save_state', path=checkpoint.as_posix())
            r.client.command('write_ram', addr=0x06001234, val=234)
        newer = bytearray(original); newer[0x1234] = 234
        check('clean exit atomically saves new native progress', native.read_bytes() == newer)
        with Runner(args.runtime, args.rom, 'hybrid', args.port, args.out / 'second', extra) as r:
            check('newer progress loads on next launch', r.memory(0x06000000) == newer)
            r.client.command('load_state', path=checkpoint.as_posix())
            check('explicit execution restore includes old SRAM', r.memory(0x06000000) == original)
        backups = list(args.out.glob('cartridge.sav.pre-state-*.bak'))
        check('newer native progress preserved before replacement',
              len(backups) == 1 and backups[0].read_bytes() == newer)
        check('restored execution saves its live SRAM', native.read_bytes() == original)
        invalid = (args.out / 'invalid.sav').resolve(); invalid.write_bytes(b'bad')
        try:
            with Runner(args.runtime, args.rom, 'hybrid', args.port, args.out / 'invalid',
                        ['--save', str(invalid)]):
                pass
        except RuntimeError:
            rejected = True
        else:
            rejected = False
        check('invalid native save rejected without overwrite', rejected and invalid.read_bytes() == b'bad')
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    (args.out / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())

"""Resolve retained caller stacks without converting wall samples to CPU costs.

Native symbols use the matching nm output; DLL frames retain module+RVA rather
than pretending the nearest export is their caller. Inclusive counts overlap.
"""
import argparse
import bisect
from collections import Counter
import json
from pathlib import Path
import struct


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('profile', type=Path)
    p.add_argument('out', type=Path)
    p.add_argument('--executable', type=Path, help='Local copy of the same hashed executable for a retained remote profile')
    a = p.parse_args()
    source = json.loads(a.profile.read_text())
    exe = a.executable or Path(source['command'][0])
    import hashlib
    raw = exe.read_bytes()
    if hashlib.sha256(raw).hexdigest() != source['exe_sha256']:
        raise ValueError('Executable identity mismatch')
    pe = struct.unpack_from('<I', raw, 0x3c)[0]
    preferred = struct.unpack_from('<Q', raw, pe + 48)[0]
    entries = []
    for line in a.profile.with_name('symbols.txt').read_text().splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) == 3 and fields[1] in ('T', 't'):
            entries.append((int(fields[0], 16), fields[2]))
    entries.sort()
    addresses = [pc for pc, _ in entries]

    def resolve(pc):
        module = next((m for m in source['modules'] if m[0] <= pc < m[1]), None)
        if module is None:
            return 'unresolved:' + hex(pc)
        if module[2].lower() == exe.name.lower():
            # Return PCs denote the next instruction, sometimes a symbol end.
            adjusted = pc - module[0] + preferred
            index = bisect.bisect_right(addresses, adjusted) - 1
            if index >= 0:
                return entries[index][1]
        return 'DLL:' + module[2] + '+' + hex(pc - module[0])

    inclusive = Counter(); chains = Counter(); depths = Counter()
    external_callers = Counter()
    for row in source.get('stacks', []):
        chain = tuple(resolve(int(pc, 16)) for pc in row['pcs'])
        n = row['samples']
        chains[chain] += n; depths[len(chain)] += n
        for name in set(chain):
            inclusive[name] += n
        if chain and chain[0].startswith('DLL:'):
            caller = next((f for f in chain[1:] if not f.startswith(('DLL:', 'unresolved:'))), 'unresolved-caller')
            external_callers[(chain[0].split('+')[0], caller)] += n
    result = dict(scope=source.get('stack_scope'), exe_sha256=source['exe_sha256'],
        samples=source['samples'], samples_with_stacks=sum(chains.values()),
        depth_histogram=dict(sorted(depths.items())),
        inclusive=[dict(function=f, samples=n) for f, n in inclusive.most_common()],
        external_callers=[dict(module=m, caller=f, samples=n) for (m, f), n in external_callers.most_common()],
        chains=[dict(functions=list(chain), samples=n) for chain, n in chains.most_common()])
    a.out.write_text(json.dumps(result, indent=2))
    print(json.dumps({k: v for k, v in result.items() if k != 'chains'}, indent=2))


if __name__ == '__main__':
    main()

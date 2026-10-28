"""Measure exact page duplication in retained agent-owned QA snapshots.

This is an offline storage opportunity estimate, not a runtime speed/memory
measurement. It excludes the file header and verifies each public payload CRC.
Page boundaries follow the existing flat serialization; actual ownership,
allocation overhead and capture/restore costs still need implementation tests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--page-size', type=int, default=4096)
    p.add_argument('snapshots', type=Path, nargs='+')
    a = p.parse_args()
    if a.page_size < 512 or a.page_size > 65536:
        p.error('Use a bounded 512..65536 byte page size')
    pool = set(); rows = []
    for path in a.snapshots:
        raw = path.read_bytes()
        if len(raw) < 112 or raw[:8] != b'VBSTATE1' or struct.unpack_from('<I', raw, 8)[0] != len(raw):
            raise ValueError('Invalid snapshot header: ' + str(path))
        if struct.unpack_from('<I', raw, 12)[0] != 1:
            raise ValueError('Expected a full public snapshot: ' + str(path))
        payload = raw[112:]
        if zlib.crc32(payload) != struct.unpack_from('<I', raw, 80)[0]:
            raise ValueError('Payload CRC mismatch: ' + str(path))
        pages = [payload[i:i+a.page_size] for i in range(0, len(payload), a.page_size)]
        unique = set(pages)
        added = unique - pool
        pool.update(unique)
        rows.append(dict(path=str(path.resolve()), sha256=hashlib.sha256(raw).hexdigest(),
            payload_bytes=len(payload), pages=len(pages), unique_pages=len(unique),
            unique_payload_bytes=sum(map(len, unique)),
            newly_unique_payload_bytes=sum(map(len, added))))
    total = sum(r['payload_bytes'] for r in rows)
    unique = sum(map(len, pool))
    result = dict(scope=__doc__, page_size=a.page_size, snapshots=rows,
        logical_payload_bytes=total, union_unique_payload_bytes=unique,
        data_only_duplicate_percent=100*(1-unique/total))
    a.out.write_text(json.dumps(result, indent=2))
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

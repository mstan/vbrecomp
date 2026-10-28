"""Attribute DLL wall samples to exact retained native call return sites.

Decode only the unambiguous five-byte relative CALL ending at a retained return
PC. Everything else stays unresolved; no nearest DLL export guesses or CPU-time
claims. Matching executable/symbol identities are mandatory.
"""
import argparse
import bisect
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('profile',type=Path);p.add_argument('out',type=Path)
    a=p.parse_args();source=json.loads(a.profile.read_text())
    exe=Path(source['command'][0]);raw=exe.read_bytes()
    assert hashlib.sha256(raw).hexdigest()==source['exe_sha256']
    # The producer hashes subprocess text before write_text's Windows newline
    # translation. Read as text to reproduce that exact normalized identity.
    symbols=a.profile.with_name('symbols.txt').read_text().encode()
    assert hashlib.sha256(symbols).hexdigest()==source['symbols_sha256']
    entries=[]
    for line in symbols.decode().splitlines():
        f=line.split(maxsplit=2)
        if len(f)==3 and f[1] in ('T','t'):entries.append((int(f[0],16),f[2]))
    entries.sort();addresses=[v for v,_ in entries]
    pe=struct.unpack_from('<I',raw,0x3c)[0]
    preferred=struct.unpack_from('<Q',raw,pe+48)[0]
    section_count=struct.unpack_from('<H',raw,pe+6)[0]
    section_table=pe+24+struct.unpack_from('<H',raw,pe+20)[0]
    sections=[]
    for i in range(section_count):
        size,rva,raw_size,offset=struct.unpack_from('<IIII',raw,section_table+i*40+8)
        sections.append((preferred+rva,raw_size,offset))
    def function(pc):
        i=bisect.bisect_right(addresses,pc)-1
        return entries[i][1] if i>=0 else 'unresolved'
    def call_target(pc):
        for start,size,offset in sections:
            if start+5<=pc<=start+size:
                at=offset+pc-start-5
                if raw[at]==0xe8:
                    target=pc+struct.unpack_from('<i',raw,at+1)[0]
                    i=bisect.bisect_right(addresses,target)-1
                    # A direct target is known only at the exact symbol entry.
                    if i>=0 and entries[i][0]==target and not entries[i][1].startswith('.'):
                        return entries[i][1]
        return 'unresolved-call'
    rows=Counter()
    for row in source.get('stacks',[]):
        pcs=[int(pc,16) for pc in row['pcs']]
        if not pcs:continue
        leaf=next((m for m in source['modules'] if m[0]<=pcs[0]<m[1]),None)
        if not leaf or leaf[2].lower()==exe.name.lower():continue
        for pc in pcs[1:]:
            module=next((m for m in source['modules'] if m[0]<=pc<m[1]),None)
            if module and module[2].lower()==exe.name.lower():
                native=pc-module[0]+preferred
                rows[(leaf[2],function(native),native,call_target(native))]+=row['samples']
                break
    result=dict(scope=source['stack_scope'],exe_sha256=source['exe_sha256'],
                symbols_sha256=source['symbols_sha256'],
                callers=[dict(module=m,caller=f,preferred_return_pc=hex(pc),callee=callee,samples=n)
                         for (m,f,pc,callee),n in rows.most_common()])
    a.out.write_text(json.dumps(result,indent=2));print(json.dumps(result,indent=2))


if __name__=='__main__':main()

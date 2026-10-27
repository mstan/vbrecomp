"""Resolve retained executable PCs using matching local DWARF, including inlines.

These remain perturbed wall-clock PC samples, not inclusive CPU-time stacks.
Unknown/DLL samples are retained in the denominator and never redistributed.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

p=argparse.ArgumentParser(description=__doc__)
for name in ('profile','exe','addr2line','out'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args()
profile=json.loads(a.profile.read_text())
raw=a.exe.read_bytes()
assert hashlib.sha256(raw).hexdigest()==profile['exe_sha256'],'Executable does not match capture'
pe=struct.unpack_from('<I',raw,0x3c)[0]
assert struct.unpack_from('<H',raw,pe+24)[0]==0x20b,'64-bit PE required'
base=struct.unpack_from('<Q',raw,pe+48)[0]
addresses=profile['executable_addresses']
counts={base+int(row['rva'],16):row['samples'] for row in addresses}
command=[str(a.addr2line),'-a','-f','-i','-C','-e',str(a.exe)]
result=subprocess.run(command,input=''.join(hex(pc)+'\n' for pc in counts),capture_output=True,text=True,check=True)
a.out.with_suffix('.addr2line.txt').write_text(result.stdout)
resolved={};current=None;pending=None
for line in result.stdout.splitlines():
    if re.fullmatch(r'0x[0-9a-fA-F]+',line):
        current=int(line,16);resolved[current]=[];pending=None
    elif pending is None:pending=line
    else:
        resolved[current].append(dict(function=pending,location=line));pending=None
functions=Counter();locations=Counter();rows=[]
for pc,count in counts.items():
    chain=resolved[pc]
    known=chain and chain[0]['function']!='??' and re.search(r':\d+(?:\s.*)?$',chain[0]['location'])
    leaf=chain[0]['function'] if known else 'unresolved-executable-source'
    functions[leaf]+=count
    if known:locations[(leaf,chain[0]['location'])]+=count
    rows.append(dict(rva=hex(pc-base),samples=count,inline_chain=chain))
def share(count):return 100*count/profile['samples']
report=dict(scope=__doc__,exe_sha256=profile['exe_sha256'],samples=profile['samples'],
    profiled_executable_samples=sum(counts.values()),
    leaf_functions=[dict(function=name,samples=n,percent=share(n)) for name,n in functions.most_common()],
    source_lines=[dict(function=f,location=l,samples=n,percent=share(n)) for (f,l),n in locations.most_common()],
    addresses=rows)
a.out.write_text(json.dumps(report,indent=2))
print(json.dumps(report['leaf_functions'][:24],indent=2))

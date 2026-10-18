"""Validate debugger architectural writes using a caller-owned game build/ROM."""
import argparse
from pathlib import Path
import tempfile
import time
from cosim import Runner

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--runtime',type=Path,required=True);p.add_argument('--rom',type=Path,required=True)
p.add_argument('--port',type=int,default=18490);args=p.parse_args()
def integer(value):return int(value,0) if isinstance(value,str) else value
with tempfile.TemporaryDirectory(prefix='vb debugger registers ') as root:
 for i,mode in enumerate(('hybrid','interpreter')):
  with Runner(args.runtime,args.rom,mode,args.port+i,Path(root)/mode) as r:
   old=r.client.command('get_registers')
   for index in (1,21,31):
    r.client.command('set_gpr',index=index,val=0xffffffff)
    assert integer(r.client.command('get_registers')['gpr'][index])==0xffffffff
    r.client.command('set_gpr',index=index,val=integer(old['gpr'][index]))
   r.client.command('pc_set',pc=0xfffffff0)
   assert integer(r.client.command('get_registers')['pc'])==0xfffffff0
   r.client.command('pc_set',pc=integer(old['pc']))
   for command,fields in [('set_gpr',dict(index=0,val=1)),('set_gpr',dict(index=32,val=1)),('set_gpr',dict(index=1,val=-1)),('set_gpr',dict(index=1,val=0x100000000)),('pc_set',dict(pc=3)),('pc_set',dict(pc=-1)),('pc_set',dict(pc=0x100000000)),('pc_set',{})]:
    try:r.client.command(command,**fields)
    except RuntimeError:pass
    else:raise AssertionError((command,fields))
   assert integer(r.client.command('get_registers')['gpr'][0])==0
   r.client.run_frames(3);assert r.client.command('execution_stats')['stopped']==0
   # Readback alone cannot prove the dispatch loop honors a changed PC. Jump
   # from a later frame back to the cartridge's original reset instruction.
   raw=args.rom.read_bytes();first=int.from_bytes(raw[-16:-14],'little');second=int.from_bytes(raw[-14:-12],'little')
   opcode=first>>10
   if opcode==0x2a:
    disp=((first&1023)<<16)|second
    if disp&(1<<25):disp-=1<<26
    target=(0xfffffff0+disp)&0xffffffff
   else:
    assert opcode==0x2f and first&31==0,'Expected reset JR or MOVHI from r0'
    target=0xfffffff4
   r.client.command('pc_set',pc=0xfffffff0)
   r.client.command('step',count=1)
   for _ in range(1000):
    regs=r.client.command('get_registers')
    if integer(regs['pc'])==target:break
    time.sleep(.005)
   else:raise AssertionError(('resume ignored PC write',regs,target))
   if opcode==0x2f:assert integer(regs['gpr'][(first>>5)&31])==(second<<16)
  print('PASS:',mode,'paused register writes, input rejection, r0 preservation and changed-PC execution')

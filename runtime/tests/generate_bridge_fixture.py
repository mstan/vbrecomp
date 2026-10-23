"""Generate a synthetic cartridge which calls a RAM routine through native code."""
from pathlib import Path
import struct
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[2]))
from recompiler.v810.analysis import RomImage,FunctionRange
from recompiler.v810.emitter import emit_full_c,emit_dispatch_c,emit_header

out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
rom=bytearray(65536)
def op(offset,primary,r1=0,r2=0,imm=None):
    struct.pack_into("<H",rom,offset,(primary<<10)|(r2<<5)|r1)
    if imm is not None: struct.pack_into("<H",rom,offset+2,imm&65535)
op(0x20,0x2b,imm=0x40)  # JAL native callee
op(0x24,0x11,2,10)      # ADD 2,r10
op(0x26,0x1a)           # HALT
op(0x60,0x06,4)         # tail JMP to RAM in r4
op(0x100,0x28,0,11,500)
op(0x104,0x28,0,1,384)
op(0x108,0x03,1,11)
struct.pack_into('<H',rom,0x10a,0x8c06) # BLT +6 (500 < 384 is false)
op(0x10c,0x10,0,10);op(0x10e,0x1a)
op(0x110,0x10,1,10);op(0x112,0x1a)
op(0x120,0x1c,24,6)    # LDSR r6,CHCW
op(0x122,0x1d,24,10)   # STSR CHCW,r10
op(0x124,0x1a)
img=RomImage.from_bytes(bytes(rom))
fns=[FunctionRange("main",0x07000020,0x07000028),FunctionRange("callee",0x07000060,0x07000062)]
fns.append(FunctionRange('branch_bounds',0x07000100,0x07000114))
fns.append(FunctionRange('cache_control',0x07000120,0x07000126))
for name,source in [("full",emit_full_c(img,fns,"bridge")),("dispatch",emit_dispatch_c(fns,"bridge",rom=img))]:
    (out/f"bridge_{name}.c").write_text(source,encoding="utf-8")
(out/"bridge.h").write_text(emit_header(fns,"bridge"),encoding="utf-8")
(out/"bridge.rom").write_bytes(rom)

"""Exercise real debugger sockets, execution controls and durable cartridge saves."""
import argparse
import json
import struct
from pathlib import Path
import time
import uuid
import traceback
from cosim import Runner
from debug_client import DebugClient


def wait_stop(client):
    end=time.monotonic()+5
    while time.monotonic()<end:
        state=client.command("execution_stats")
        if state["stopped"]: return state
        time.sleep(.005)
    raise TimeoutError("instruction control failed to stop")


def validate(args,mode,port):
    checks=[]
    with Runner(args.oracle if mode=="oracle" else args.runtime,args.rom,mode,port,args.out/mode) as runner:
        c=runner.client
        capability=c.command("capabilities")
        assert capability["protocol"]==2 and capability["trace_version"]==2
        assert capability["max_read_bytes"]==65536
        assert capability["instruction_control"]==(mode!="oracle")
        assert set(("pause","run_frames","read_ram","write_ram","cpuhook","audio_pcm")) <= set(capability["commands"])
        checks.append("advertised protocol and supported controls")
        # A JSON line split across packets must remain buffered.
        c.socket.sendall(b'{"cmd":"pi');time.sleep(.05)
        c.socket.sendall(b'ng","id":9001}\r\n{"cmd":"frame","id":9002}\n')
        replies=[json.loads(c.file.readline()) for _ in range(2)]
        assert [r["id"] for r in replies]==[9001,9002] and replies[1]["frame"]==0
        checks.append("fragmentation, CRLF, pipelining and IDs")
        for mask in (4,0x1000,0x8000,0):
            c.command("set_input",pad=mask)
            assert int(c.command("pad_state")["pad"],0)==(mask|2)
        checks.append("hardware input mask round trip")
        if mode!="oracle":
            before=c.command("execution_stats")
            c.command("step",count=1);after=wait_stop(c)
            assert after["native_instructions"]+after["interpreted"]-before["native_instructions"]-before["interpreted"]==1
            assert int(c.command("get_registers")["pc"],0)==0xfffffff4
            c.command("breakpoint",pc=0xfffffff8);c.command("continue");wait_stop(c)
            assert int(c.command("get_registers")["pc"],0)==0xfffffff8
            c.command("breakpoint",pc=-1)
            c.command("step",count=100);after=wait_stop(c)
            assert after["native_instructions"]+after["interpreted"]==102
            checks.append("single/multiple instruction step and breakpoint")
        c.run_frames(2)
        first=c.command("get_registers");time.sleep(.08);second=c.command("get_registers")
        assert {k:v for k,v in first.items() if k!="id"}=={k:v for k,v in second.items() if k!="id"}
        checks.append("exact frame advance and stable pause")
        c.run_frames(40)
        trace=c.command("cpuhook",start=0,max=2)
        assert trace["resident_lo"]>0 and trace["begin"]==trace["resident_lo"]
        records=list(struct.iter_unpack("<IIIIQ",bytes.fromhex(trace["hex"])))
        assert len(records)==2 and all(row[3]==2 for row in records)
        again=c.command("cpuhook",start=trace["begin"]+1,max=1)
        assert again["hex"]==trace["hex"][48:]
        checks.append("continuous trace eviction, absolute indexing and version")
        for base in (0x05000000,0x06000000):
            c.command("write_ram",addr=base+123,val=0xa5)
            data=bytes.fromhex(c.command("read_ram",addr=base,len=65536)["hex"])
            assert len(data)==65536 and data[123]==0xa5
        checks.append("full WRAM/SRAM reads and paused writes")
        # Delay reading a multi-megabyte response; the server must retain all
        # unsent bytes and frame the following response independently.
        c.socket.sendall(b'{"cmd":"cpuhook","id":9003,"start":0,"max":65536}\n{"cmd":"ping","id":9004}\n')
        time.sleep(.2)
        trace=json.loads(c.file.readline());ping=json.loads(c.file.readline())
        assert trace["id"]==9003 and len(bytes.fromhex(trace["hex"]))==65536*24 and ping["id"]==9004
        checks.append("large response backpressure and following request")
        try: c.command("read_ram",addr=-1,len=16)
        except RuntimeError: pass
        else: raise AssertionError("invalid read accepted")
        c.command("ping")
        c.close();runner.client=None;time.sleep(.03)
        runner.client=DebugClient(port);runner.client.command("ping")
        checks.append("invalid request isolation and reconnect")
    assert runner.process.returncode==0
    checks.append("clean acknowledged quit")
    return checks


def saves(args):
    directory=args.out/"saves";directory.mkdir(parents=True,exist_ok=True)
    path=(directory/(uuid.uuid4().hex+".sav")).resolve()
    for expected in (0,0x5a):
        with Runner(args.runtime,args.rom,"hybrid",args.port,directory,extra=("--save",str(path))) as runner:
            assert runner.memory(0x06001234,1)==bytes([expected])
            runner.client.command("write_ram",addr=0x06001234,val=0x5a)
        assert runner.process.returncode==0 and path.stat().st_size==65536
    bad=directory/(uuid.uuid4().hex+"-invalid.sav");bad.write_bytes(b"short")
    try:
        with Runner(args.runtime,args.rom,"hybrid",args.port,directory/"invalid",extra=("--save",str(bad.resolve()))):
            raise AssertionError("truncated save accepted")
    except RuntimeError: pass
    assert bad.read_bytes()==b"short"
    return ["64KiB save round trip across processes","truncated save rejected without overwrite"]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ("runtime","oracle","rom","out"): p.add_argument("--"+name,type=Path,required=True)
    p.add_argument("--port",type=int,default=4550);args=p.parse_args()
    args.out.mkdir(parents=True,exist_ok=True);report={"passed":False,"checks":{}}
    try:
        for i,mode in enumerate(("hybrid","interpreter","oracle")):
            report["checks"][mode]=validate(args,mode,args.port+i)
        report["checks"]["saves"]=saves(args);report["passed"]=True
    except Exception as exc: report["error"]=repr(exc);report["traceback"]=traceback.format_exc()
    (args.out/"report.json").write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2));return 0 if report["passed"] else 1


if __name__=="__main__": raise SystemExit(main())

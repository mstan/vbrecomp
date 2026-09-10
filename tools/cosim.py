"""Deterministic cross-process VB comparison, with byte-audited mismatch reports.

No state is copied between machines. Each runs the same frame/input schedule.
Select comparison planes explicitly: CPU state at frame boundaries is a strict
timing comparison; video-only results never certify CPU or memory parity.
"""
from __future__ import annotations
import argparse
from contextlib import ExitStack
import hashlib
import json
import os
import socket
from pathlib import Path
import subprocess
import struct
import time
from debug_client import DebugClient
from _imgio import load_png


class Runner:
    def __init__(self, exe, rom, mode, port, directory, extra=()):
        self.directory=Path(directory).resolve();self.directory.mkdir(parents=True,exist_ok=True)
        self.exe=Path(exe).resolve();self.rom=Path(rom).resolve()
        command=[str(self.exe),"--rom",str(self.rom),"--headless","--paused","--port",str(port),*extra]
        if mode!="oracle":
            command += ["--execution",mode]
            command += ["--config",str(self.directory/"settings.cfg"),"--mods-dir",str(self.directory/"mods")]
            if "--save" not in extra: command += ["--no-save"]
        environment={k:v for k,v in os.environ.items() if not k.startswith("VBRECOMP_")}
        # Refuse occupied ports before starting; never attach to another run.
        with socket.socket() as probe:
            if os.name=="nt": probe.setsockopt(socket.SOL_SOCKET,socket.SO_EXCLUSIVEADDRUSE,1)
            probe.bind(("127.0.0.1",port))
        self.process=subprocess.Popen(command,cwd=self.directory,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
            env=environment,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
        self.client=None
        deadline=time.monotonic()+15
        try:
            while time.monotonic()<deadline:
                if self.process.poll() is not None: raise RuntimeError("runner exited during startup")
                try: self.client=DebugClient(port,timeout=15);break
                except OSError: time.sleep(.02)
            if self.client is None: raise TimeoutError("runner startup")
            self.client.command("ping")
            if self.client.command("frame")["frame"] != 0: raise RuntimeError("--paused did not stop at reset")
        except BaseException:
            self.close();raise

    def __enter__(self): return self
    def __exit__(self,*args):
        self.close()
        if args[0] is None and self.process.returncode:
            raise RuntimeError(f"runner exit {self.process.returncode}; see {self.directory/'crash.json'}")
    def close(self):
        if self.client:
            try: self.client.command("quit")
            except Exception: pass
            self.client.close();self.client=None
        try: out,err=self.process.communicate(timeout=4)
        except subprocess.TimeoutExpired:
            self.process.terminate();out,err=self.process.communicate(timeout=4)
        if self.process.returncode:
            (self.directory/"crash.json").write_text(json.dumps({"exit":self.process.returncode,"report":err.decode(errors="replace")[-4000:]},indent=2))

    def memory(self, base, count=65536):
        data=bytearray()
        for off in range(0,count,65536):
            n=min(65536,count-off)
            reply=self.client.command("read_ram",addr=base+off,len=n)
            chunk=bytes.fromhex(reply["hex"])
            if len(chunk)!=n: raise RuntimeError(f"short memory read at {base+off:08X}")
            data.extend(chunk)
        return bytes(data)

    def snapshot(self, frame, planes):
        result={}
        if "audio" in planes:
            # Match absolute sample indices, leaving room for each frontend's
            # independent Blip output batching. Never slide waves to align them.
            end=max(0,frame*397824*44100//20000000-1024)
            start=max(0,end-4096)
            if end>start:
                pcm=self.client.command("audio_pcm",start=start,max=end-start)
                if pcm["begin"]!=start or pcm["returned"]!=end-start:
                    raise RuntimeError("requested audio window unavailable")
                if (pcm["rate"],pcm["channels"],pcm["format"])!=(44100,2,"s16le"):
                    raise RuntimeError("incompatible audio format")
                result["audio"]=bytes.fromhex(pcm["hex"])
                if len(result["audio"])!=(end-start)*4: raise RuntimeError("short audio capture")
            else: result["audio"]=b""
        if "cpu" in planes:
            cpu=self.client.command("get_registers")
            # Common architectural fields only; cycle counts are reported by
            # trace comparison, not inferred from the frontend frame number.
            result["cpu"]={k:cpu[k] for k in ("pc","psw","gpr","eipc","eipsw","fepc","fepsw","ecr")}
        for name,base in [("wram",0x05000000),("sram",0x06000000)]:
            if name in planes: result[name]=self.memory(base)
        if "devices" in planes:
            schema=json.loads(Path(__file__).with_name("device_schema.json").read_text())
            raw=self.client.command("device_state")
            if raw["schema"]!=1: raise RuntimeError("device schema 1 required")
            fields={}
            for group,names in schema.items():
                if len(raw[group])!=len(names): raise RuntimeError("incomplete device state: "+group)
                fields.update({group+"."+name:value for name,value in zip(names,raw[group])})
            result["devices"]=fields
        if "vram" in planes: result["vram"]=self.memory(0,0x40000)
        if "vip" in planes:
            vip=self.client.command("vip_state")
            result["vip"]={k:vip[k] for k in ("intpnd","intenb","dpctrl","xpctrl","frmcyc","bkcol","brta","brtb","brtc","rest")}
        for plane,eye in (("video",0),("video_right",1),("presented",0)):
            if plane not in planes: continue
            path=self.directory/f"frame-{frame}-{plane}.png"
            self.client.command("screenshot",path=path.as_posix(),eye=eye,presented=int(plane=="presented"))
            width,height,pixels=load_png(path)
            result[plane]=width.to_bytes(4,"little")+height.to_bytes(4,"little")+b"".join((p&0xffffff).to_bytes(3,"little") for p in pixels)
        return result


def compare_snapshots(a,b):
    differences=[]
    for key in a:
        left,right=a[key],b[key]
        if isinstance(left,bytes):
            hashes=[hashlib.sha256(x).hexdigest() for x in (left,right)]
            # Always audit hash agreement against bytes, including the
            # equality case; a broken hasher cannot certify a false pass.
            if (hashes[0]==hashes[1]) != (left==right): raise RuntimeError("hash/byte audit failed")
            if left!=right:
                common=min(len(left),len(right))
                offset=next((i for i in range(common) if left[i]!=right[i]),common)
                differences.append({"plane":key,"first_byte":offset,"bytes_different":sum(x!=y for x,y in zip(left,right))+abs(len(left)-len(right)),
                                    "a_byte":left[offset] if offset<len(left) else None,"b_byte":right[offset] if offset<len(right) else None,"sha256":hashes})
        elif left!=right:
            differences.append({"plane":key,"fields":{field:[left[field],right[field]] for field in left if left[field]!=right[field]}})
    return differences


def trace_compare(args):
    """Compare an absolute resident instruction/IRQ window without skipping mismatches."""
    record=struct.Struct("<IIIIQ")
    selected=[("pc","psw","fnv","version","cycle").index(x) for x in args.trace_fields.split(",")]
    report={"passed":False,"compared":0,"first_divergence":None,"first_cycle_difference":None,
            "scope":"pre-instruction and IRQ boundaries: PC, full PSW, FNV of r1..r31; continuous history v2"}
    report.update({"rom_sha256":hashlib.sha256(args.rom.read_bytes()).hexdigest(),
                   "runtime_sha256":hashlib.sha256(args.runtime.read_bytes()).hexdigest(),
                   "oracle_sha256":hashlib.sha256(args.oracle.read_bytes()).hexdigest(),
                   "start":args.start,"requested":args.instructions,"fields":args.trace_fields.split(",")})
    args.out.mkdir(parents=True,exist_ok=True)
    try:
        with Runner(args.runtime,args.rom,args.a_mode,args.port,args.out/"a") as a, \
             Runner(args.oracle,args.rom,"oracle",args.port+1,args.out/"b") as b:
            schedule=json.loads(args.route.read_text()) if args.route else [{"frames":args.frames,"pad":0}]
            for segment in schedule:
                for runner in (a,b):
                    runner.client.command("set_input",pad=segment.get("pad",0))
                    runner.client.run_frames(segment["frames"])
            if args.tail:
                heads=[runner.client.command("cpuhook",start=0,max=1) for runner in (a,b)]
                args.start=max(max(h["resident_lo"] for h in heads),min(h["head"] for h in heads)-args.instructions)
            cursor=args.start
            while cursor<args.start+args.instructions:
                replies=[r.client.command("cpuhook",start=cursor,max=min(65536,args.start+args.instructions-cursor)) for r in (a,b)]
                if any(r["begin"]>cursor for r in replies): raise RuntimeError("requested trace prefix was evicted")
                streams=[list(record.iter_unpack(bytes.fromhex(r["hex"]))) for r in replies]
                if any(row[3]!=2 for stream in streams for row in stream): raise RuntimeError("CPU trace version 2 required on both sides")
                count=min(map(len,streams))
                if not count: break
                for i,(left,right) in enumerate(zip(*streams)):
                    seq=cursor+i
                    if left[4]!=right[4] and report["first_cycle_difference"] is None:
                        report["first_cycle_difference"]={"seq":seq,"a":left,"b":right}
                    if any(left[k]!=right[k] for k in selected):
                        report["first_divergence"]={"seq":seq,"a":left,"b":right,
                            "context_a":streams[0][max(0,i-8):i+9],"context_b":streams[1][max(0,i-8):i+9]}
                        break
                    report["compared"]+=1
                if report["first_divergence"]: break
                cursor+=count
            report["requested"]=args.instructions
            report["start"]=args.start
            report["fields"]=args.trace_fields.split(",")
            report["passed"]=report["compared"]==args.instructions and report["first_divergence"] is None
    except Exception as exc:
        report["passed"]=False;report["error"]=str(exc)
    (args.out/"trace.json").write_text(json.dumps(report,indent=2))
    return report


def compare(args, a_mode, b_mode, directory, planes, inject=False):
    directory=Path(directory);directory.mkdir(parents=True,exist_ok=True)
    schedule=json.loads(args.route.read_text()) if args.route else [{"frames":1,"pad":0} for _ in range(args.frames)]
    if not isinstance(schedule,list) or not schedule or any(
        not isinstance(segment,dict) or not isinstance(segment.get("frames"),int) or segment["frames"]<1 or
        not isinstance(segment.get("pad",0),int) or not 0<=segment.get("pad",0)<=65535 for segment in schedule):
        raise ValueError("route must contain positive frame counts and 16-bit pad masks")
    report={"a_mode":a_mode,"b_mode":b_mode,"planes":planes,"rom_sha256":hashlib.sha256(args.rom.read_bytes()).hexdigest(),
            "runtime_sha256":hashlib.sha256(args.runtime.read_bytes()).hexdigest(),"checkpoints":0,"first_divergence":None,"byte_audits":0}
    if b_mode=="oracle": report["oracle_sha256"]=hashlib.sha256(args.oracle.read_bytes()).hexdigest()
    try:
        with ExitStack() as stack:
            a=stack.enter_context(Runner(args.oracle if a_mode=="oracle" else args.runtime,args.rom,a_mode,args.port,directory/"a"))
            b=stack.enter_context(Runner(args.oracle if b_mode=="oracle" else args.runtime,args.rom,b_mode,args.port+1,directory/"b"))
            frame=0
            for i,segment in enumerate(schedule):
                count=segment["frames"];pad=segment.get("pad",0)
                for runner in (a,b): runner.client.command("set_input",pad=pad);runner.client.run_frames(count)
                frame+=count
                if inject and i==0:
                    value=b.memory(0x0500fff0,1)[0]^0xff
                    b.client.command("write_ram",addr=0x0500fff0,val=value)
                left,right=a.snapshot(frame,planes),b.snapshot(frame,planes)
                diffs=compare_snapshots(left,right)
                report["byte_audits"]+=sum(isinstance(v,bytes) for v in left.values())
                report["checkpoints"]+=1
                if diffs:
                    report["first_divergence"]={"frame":frame,"previous_checkpoint":frame-count,"differences":diffs}
                    for name,state in [("a",left),("b",right)]:
                        for plane,value in state.items():
                            if isinstance(value,bytes): (directory/f"{name}-{plane}.bin").write_bytes(value)
                    break
            for name,runner in [("a",a),("b",b)]:
                if (a_mode if name=="a" else b_mode)!="oracle": report[name+"_execution"]=runner.client.command("execution_stats")
        report["passed"]=report["checkpoints"]>0 and report["first_divergence"] is None
    except Exception as exc:
        report["passed"]=False;report["error"]=str(exc)
    (directory/"report.json").write_text(json.dumps(report,indent=2))
    return report


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("action",choices=["compare","gates","trace"])
    p.add_argument("--runtime",type=Path,required=True);p.add_argument("--oracle",type=Path)
    p.add_argument("--rom",type=Path,required=True);p.add_argument("--out",type=Path,required=True)
    p.add_argument("--route",type=Path);p.add_argument("--frames",type=int,default=10)
    p.add_argument("--port",type=int,default=4490)
    p.add_argument("--instructions",type=int,default=100000)
    p.add_argument("--start",type=int,default=0,help="absolute trace sequence to compare")
    p.add_argument("--tail",action="store_true",help="compare the latest common absolute trace window")
    p.add_argument("--trace-fields",default="pc,psw,fnv,cycle")
    p.add_argument("--a-mode",choices=["hybrid","native","interpreter"],default="hybrid")
    p.add_argument("--b-mode",choices=["hybrid","native","interpreter","oracle"],default="oracle")
    p.add_argument("--planes",default="cpu,wram,sram,vram,vip,devices,video,video_right,presented,audio")
    args=p.parse_args();planes=args.planes.split(",")
    if args.frames<1 or any(x not in ("cpu","wram","sram","vram","vip","devices","video","video_right","presented","audio") for x in planes): p.error("invalid frames or planes")
    if args.start<0 or args.instructions<1 or any(field not in ("pc","psw","fnv","cycle") for field in args.trace_fields.split(",")): p.error("invalid trace range or fields")
    if args.action=="trace":
        if not args.oracle: p.error("--oracle required")
        report=trace_compare(args)
    elif args.action=="compare":
        if args.b_mode=="oracle" and not args.oracle: p.error("--oracle required for oracle comparison")
        report=compare(args,args.a_mode,args.b_mode,args.out,planes)
    else:
        args.out.mkdir(parents=True,exist_ok=True)
        selfcheck=compare(args,"hybrid","hybrid",args.out/"determinism",planes)
        referencecheck=compare(args,"interpreter","interpreter",args.out/"reference-determinism",planes)
        bridge=compare(args,"hybrid","interpreter",args.out/"interpreter",planes)
        fault=compare(args,"hybrid","hybrid",args.out/"injected-fault",["wram"],True)
        divergence=fault.get("first_divergence")
        caught=bool(divergence and divergence["differences"][0]["first_byte"]==65520 and divergence["differences"][0]["bytes_different"]==1)
        report={"passed":selfcheck["passed"] and referencecheck["passed"] and bridge["passed"] and caught,"determinism":selfcheck,"reference_determinism":referencecheck,"bridge":bridge,"injected_fault_detected":caught,"fault":fault}
        if args.oracle:
            oraclecheck=compare(args,"oracle","oracle",args.out/"oracle-determinism",planes)
            report["oracle_determinism"]=oraclecheck
            report["passed"] &= oraclecheck["passed"]
        (args.out/"gates.json").write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2));return 0 if report["passed"] else 1


if __name__=="__main__": raise SystemExit(main())

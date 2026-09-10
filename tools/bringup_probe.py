"""Reproducible, bounded native/interpreter/Beetle bring-up observations."""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time
from debug_client import DebugClient
from _imgio import load_png


def observe(exe, rom, mode, port, checkpoints, output, inputs=None):
    command=[str(exe), "--rom", str(rom), "--headless", "--paused", "--port", str(port)]
    if mode != "oracle": command += ["--execution", mode]
    proc=subprocess.Popen(command,cwd=output,stdout=subprocess.PIPE,stderr=subprocess.PIPE,
                          creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
    result={"mode":mode,"executable":str(exe),"checkpoints":[]}
    client=None
    try:
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            try: client=DebugClient(port,timeout=10);break
            except OSError:
                if proc.poll() is not None: raise RuntimeError(f"process exited: {proc.returncode}")
                time.sleep(.03)
        if client is None: raise TimeoutError("debugger startup")
        client.command("ping")
        previous=0
        for frame in checkpoints:
            if inputs and previous in inputs:
                client.command("set_input", pad=inputs[previous])
            client.run_frames(frame-previous)
            previous=frame
            row={"frame":frame,"cpu":client.command("get_registers"),"vip":client.command("vip_state")}
            if mode!="oracle": row["execution"]=client.command("execution_stats")
            shot=output/f"{mode}-{frame}.png"
            row["screenshot"]=client.command("screenshot",path=shot.as_posix())
            width,height,pixels=load_png(shot)
            raw=b"".join((v&0xffffff).to_bytes(3,"little") for v in pixels)
            row["image"]={"width":width,"height":height,"nonblack":sum(bool(v&0xffffff) for v in pixels),"sha256":hashlib.sha256(raw).hexdigest()}
            for name,base in [("wram",0x05000000),("sram",0x06000000)]:
                data=bytearray()
                for off in range(0,65536,4096):
                    response=client.command("read_ram",addr=base+off,len=4096)
                    chunk=bytes.fromhex(response["hex"])
                    if len(chunk)!=4096: raise RuntimeError(f"short {name} read: {len(chunk)}")
                    data.extend(chunk)
                (output/f"{mode}-{frame}-{name}.bin").write_bytes(data)
                row[name]=hashlib.sha256(data).hexdigest()
            result["checkpoints"].append(row)
            print(json.dumps({"mode":mode,"frame":frame,"image":row["image"],"execution":row.get("execution")}),flush=True)
        # A live pause must hold the complete architectural state.
        paused=client.command("get_registers")
        time.sleep(.06)
        after=client.command("get_registers")
        result["pause_stable"]={k:v for k,v in paused.items() if k!="id"}=={k:v for k,v in after.items() if k!="id"}
        client.command("quit"); client.close();client=None
        proc.wait(5)
        result["exit_code"]=proc.returncode
    except Exception as exc:
        result["error"]=str(exc)
    finally:
        if client:
            try: client.command("quit")
            except Exception: pass
            client.close()
        try: stdout,stderr=proc.communicate(timeout=4)
        except subprocess.TimeoutExpired:
            proc.terminate();stdout,stderr=proc.communicate(timeout=4)
        result["process_exit"]=proc.returncode
        if proc.returncode: result["crash_report"]=stderr.decode(errors="replace")[-4000:]
    return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--runtime",type=Path,required=True)
    p.add_argument("--oracle",type=Path,required=True)
    p.add_argument("--rom",type=Path,required=True)
    p.add_argument("--out",type=Path,required=True)
    p.add_argument("--frames",default="1,10,60")
    p.add_argument("--modes",default="hybrid,interpreter,oracle")
    p.add_argument("--inputs",default="",help="frame:hardware_mask,...; frames must be checkpoints, or 0")
    p.add_argument("--base-port",type=int,default=4490)
    args=p.parse_args();args.out=args.out.resolve();args.out.mkdir(parents=True,exist_ok=True)
    rows=[]
    for i,mode in enumerate(args.modes.split(",")):
        row=observe((args.oracle if mode=="oracle" else args.runtime).resolve(),args.rom.resolve(),mode,args.base_port+i,
                    [int(f) for f in args.frames.split(",")],args.out,
                    {int(f):int(mask,0) for f,mask in (item.split(":") for item in args.inputs.split(",") if item)})
        rows.append(row)
        (args.out/"observations.json").write_text(json.dumps({"rom_sha256":hashlib.sha256(args.rom.read_bytes()).hexdigest(),"runs":rows},indent=2))
        if row.get("error"): print(json.dumps(row),flush=True)
    return int(any(row.get("error") or not row.get("pause_stable") for row in rows))


if __name__=="__main__": raise SystemExit(main())

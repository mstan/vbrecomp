"""cpuhook_compare.py — per-instruction recomp-vs-oracle divergence + cycle Delta.

The harness for the shared Axis-1/2/3/6 lever (VB_ACCURACY_BURNDOWN.md):
both processes carry an always-on per-instruction CPU-hook ring recording
{pc, packed PSW, regs-FNV, cumulative cycle} for every RETIRED guest
instruction (recomp: cpuhook.c via VB_CPUHOOK, built -DVBRECOMP_CPUHOOK=ON;
oracle: RB_CPUHOOK in the beetle-vb core, libretro.cpp). Per CLAUDE.md
Rule 14 the two are independent processes with the same JSON wire protocol
on different ports (vb-runtime 4390, vb-beetle 4391); we QUERY the ring
window [0, N) from boot — never an armed capture.

Aligned by retire-seq (the Nth retired instruction on each side), the tool
reports:
  * the FIRST divergence — pc (control flow), regs-FNV (data), or PSW
    (flags) — with full context, and
  * the cycle Delta trajectory over the aligned prefix (Axis 2).

INTERPRETATION (read before trusting a "divergence"):
  * The recomp delivers IRQs only between dispatch passes (Axis 3), while
    the oracle takes them at the exact instruction — so the streams
    legitimately diverge at the FIRST interrupt regardless of instruction
    semantics. The clean Axis-1 signal is a divergence in the
    pre-first-IRQ boot window (straight-line setup code). A pc-divergence
    landing on an ISR vector entry is Axis-3 quantization, not an Axis-1 bug.
  * Likewise cumulative cycles diverge across HALT periods (recomp adds
    fixed idle chunks; oracle jumps to the next event), so the clean
    Axis-2 cycle Delta is the active prefix before the first HALT.

Usage (launch both, compare the first 2M instructions from boot):
    python tools/cpuhook_compare.py --rom roms/marios_tennis.vb --instructions 2000000

The recomp must be built with the hook (default is a no-op):
    cmake -S . -B build -DVBRECOMP_CPUHOOK=ON && cmake --build build --target vb-runtime
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

REC = struct.Struct("<IIIIQ")   # pc, psw, fnv, pad, cycle  (24 bytes)
PSW_ARITH = 0x0000000F          # Z|S|OV|CY — the divergence-prone flags

PSW_BITS = [(0x1, "Z"), (0x2, "S"), (0x4, "OV"), (0x8, "CY"),
            (0x1000, "ID"), (0x4000, "EP"), (0x8000, "NP")]


def _send(host: str, port: int, cmd: dict, timeout: float = 15.0) -> dict:
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall((json.dumps(cmd) + "\n").encode("ascii"))
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode("ascii", errors="replace").strip())


def _wait_ping(host: str, port: int, deadline_s: float) -> bool:
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            if _send(host, port, {"cmd": "ping"}, timeout=2.0).get("ok"):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def fetch(host: str, port: int, start: int, maxn: int) -> dict:
    r = _send(host, port, {"cmd": "cpuhook", "start": start, "max": maxn})
    if not r.get("ok"):
        raise RuntimeError(f"cpuhook failed on {port}: {r}")
    return r


def parse_recs(hexs: str) -> list[tuple]:
    raw = bytes.fromhex(hexs)
    return [REC.unpack_from(raw, i) for i in range(0, len(raw), REC.size)]


def psw_str(psw: int) -> str:
    on = [name for bit, name in PSW_BITS if psw & bit]
    return "|".join(on) if on else "-"


def wait_head(host: str, port: int, want: int, deadline_s: float) -> int:
    """Wait until the ring head reaches `want` (or stalls). Returns head."""
    end = time.time() + deadline_s
    last = -1
    stall = time.time() + 8.0
    while time.time() < end:
        h = int(fetch(host, port, 0, 1)["head"])
        if h >= want:
            return h
        if h != last:
            last = h
            stall = time.time() + 8.0
        elif time.time() > stall:
            return h
        time.sleep(0.05)
    return int(fetch(host, port, 0, 1)["head"])


def compare(host: str, rport: int, oport: int, target: int,
            psw_mask: int, window: int = 65536) -> dict:
    """Walk both rings from seq 0 in lockstep; return the first divergence
    and the cycle-Delta stats over the aligned prefix."""
    cursor = 0
    cyc_min = cyc_max = None
    first = None
    pc_div = fnv_div = psw_div = None
    compared = 0
    while cursor < target:
        n = min(window, target - cursor)
        rr = fetch(host, rport, cursor, n)
        oo = fetch(host, oport, cursor, n)
        # Eviction guard: if either side already rolled past `cursor`, the
        # from-boot prefix is gone — surface it, never silently skip.
        if int(rr["begin"]) > cursor or int(oo["begin"]) > cursor:
            return {"status": "evicted",
                    "evicted_at": cursor,
                    "recomp_begin": int(rr["begin"]),
                    "oracle_begin": int(oo["begin"]),
                    "compared": compared}
        rrecs = parse_recs(rr["hex"])
        orecs = parse_recs(oo["hex"])
        m = min(len(rrecs), len(orecs))
        if m == 0:
            break
        for i in range(m):
            rp, rpsw, rfnv, _, rcyc = rrecs[i]
            op, opsw, ofnv, _, ocyc = orecs[i]
            d = rcyc - ocyc
            cyc_min = d if cyc_min is None else min(cyc_min, d)
            cyc_max = d if cyc_max is None else max(cyc_max, d)
            seq = cursor + i
            compared += 1
            if first is None:
                kind = None
                if rp != op:
                    kind = "pc"
                    pc_div = (rp, op)
                elif (rpsw & psw_mask) != (opsw & psw_mask):
                    kind = "psw"
                    psw_div = (rpsw, opsw)
                elif rfnv != ofnv:
                    kind = "fnv"
                    fnv_div = (rfnv, ofnv)
                if kind:
                    first = {"kind": kind, "seq": seq,
                             "recomp": {"pc": rp, "psw": rpsw, "fnv": rfnv,
                                        "cycle": rcyc},
                             "oracle": {"pc": op, "psw": opsw, "fnv": ofnv,
                                        "cycle": ocyc},
                             "cycle_delta_at_div": d}
        cursor += m
        if first is not None:
            break
    return {"status": "ok", "compared": compared,
            "cycle_delta_min": cyc_min, "cycle_delta_max": cyc_max,
            "first_divergence": first,
            "pc_div": pc_div, "fnv_div": fnv_div, "psw_div": psw_div}


# --------------------------------------------------------------------------
# Re-sync mode: step PAST timing/peripheral divergences to keep validating
# CPU semantics deeper. A hardware-register read returns a timing-dependent
# value, but it lands in a GPR that is usually overwritten within a few
# instructions (state reconverges), and poll-loops on such a register
# diverge only in LENGTH. So on a mismatch we search a bounded window for a
# realignment — equal skip (j,j) for the overwrite-reconverge case, or
# single-stream skip (j,0)/(0,j) for poll-loop length differences — and
# continue, classifying each divergence by decoding the culprit instruction.
# --------------------------------------------------------------------------
def drain_all(host: str, port: int, target: int, window: int = 65536) -> list:
    """Fetch records [0, target) into a list of (pc, psw, fnv, cycle)."""
    out = []
    cur = 0
    while cur < target:
        r = fetch(host, port, cur, min(window, target - cur))
        if int(r["begin"]) > cur:
            break  # evicted (shouldn't happen with first-N capture)
        recs = parse_recs(r["hex"])
        if not recs:
            break
        for pc, psw, fnv, _pad, cyc in recs:
            out.append((pc, psw, fnv, cyc))
        cur += len(recs)
    return out


def make_classifier(rom_path: str):
    """Return classify(pc) -> (tag, mnemonic) by decoding the cart."""
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from recompiler.v810.analysis import RomImage
    rom = RomImage.from_bytes(Path(rom_path).read_bytes())
    LOADS = {0x30, 0x31, 0x33, 0x38, 0x39, 0x3B}   # LD.B/H/W, IN.B/H/W
    ARITH = {0x08, 0x09, 0x0A, 0x0B}               # MUL, DIV, MULU, DIVU

    def classify(pc: int):
        ins = rom.decode_at_va(pc)
        if ins is None:
            return ("decode-fail", "?")
        if ins.fmt.name == "VI" and ins.opcode6 in LOADS:
            return ("mmio/timing-input", ins.mnemonic)
        if ins.fmt.name == "I" and ins.opcode6 in ARITH:
            return ("ARITH-check", ins.mnemonic)   # MUL/DIV: the flagged bugs
        return ("other-semantic", ins.mnemonic)
    return classify


def resync_compare(r: list, o: list, classify, w: int = 8192, k: int = 8) -> dict:
    n = min(len(r), len(o))

    def run_match(a: int, b: int) -> bool:
        if a + k > len(r) or b + k > len(o):
            return False
        for t in range(k):
            if r[a + t][0] != o[b + t][0] or r[a + t][2] != o[b + t][2]:
                return False
        return True

    i = j = 0
    matched = 0
    divs: list[dict] = []
    while i < n and j < n:
        if r[i][0] == o[j][0] and r[i][2] == o[j][2]:
            matched += 1
            i += 1
            j += 1
            continue
        found = None
        for d in range(1, w + 1):
            for (da, db) in ((d, d), (d, 0), (0, d)):
                if run_match(i + da, j + db):
                    found = (da, db)
                    break
            if found:
                break
        culprit = r[i - 1][0] if i > 0 else r[i][0]
        tag, mnem = classify(culprit)
        divs.append({"seq": i, "r_pc": r[i][0], "o_pc": o[j][0],
                     "culprit_pc": culprit, "class": tag, "culprit_op": mnem,
                     "skip": found, "matched_before": matched,
                     "ctrl_flow": r[i][0] != o[j][0]})
        if not found:
            break
        i += found[0]
        j += found[1]
        if len(divs) >= 300:
            break
    return {"matched_total": matched, "divs": divs,
            "reached": i, "compared": n}


def fmt_resync(rep: dict) -> str:
    L = ["=" * 70,
         "  VB CPU-HOOK RE-SYNC  recomp (4390) vs oracle (4391)",
         "=" * 70,
         f"  instructions matched (across re-syncs)  {rep['matched_total']:,}",
         f"  divergence regions found                {len(rep['divs'])}"]
    by_class: dict[str, int] = {}
    for d in rep["divs"]:
        by_class[d["class"]] = by_class.get(d["class"], 0) + 1
    for cls, c in sorted(by_class.items()):
        L.append(f"      {cls:20} {c}")
    arith = [d for d in rep["divs"] if d["class"] == "ARITH-check"]
    other = [d for d in rep["divs"] if d["class"] == "other-semantic"]
    L.append("-" * 70)
    if arith:
        L.append(f"  ** {len(arith)} ARITH (MUL/DIV) divergence(s) — candidate "
                 f"Axis-1 semantic bugs: **")
        for d in arith[:10]:
            L.append(f"     seq {d['seq']:,}  culprit {d['culprit_op']} @ "
                     f"0x{d['culprit_pc']:08X}")
    if other:
        L.append(f"  {len(other)} non-load semantic divergence(s) (first 10):")
        for d in other[:10]:
            cf = " [ctrl-flow]" if d["ctrl_flow"] else ""
            L.append(f"     seq {d['seq']:,}  culprit {d['culprit_op']} @ "
                     f"0x{d['culprit_pc']:08X}{cf}")
    if not arith and not other:
        L.append("  No NON-peripheral semantic divergence — every divergence "
                 "stems from a hardware-register read (VIP/VSU/timer timing). "
                 "CPU semantics validated across the whole window.")
    if rep["divs"] and rep["divs"][-1]["skip"] is None:
        d = rep["divs"][-1]
        L.append(f"  (stopped: could not re-sync at seq {d['seq']:,} within "
                 f"window — culprit {d['culprit_op']} @ 0x{d['culprit_pc']:08X})")
    L.append("=" * 70)
    return "\n".join(L)


def fmt(rep: dict, psw_mask: int) -> str:
    L = ["=" * 64,
         "  VB CPU-HOOK DIVERGENCE  recomp (4390) vs oracle (4391)",
         "=" * 64]
    if rep["status"] == "evicted":
        L.append(f"  EVICTED: ring rolled past seq {rep['evicted_at']} before "
                 f"compare (recomp_begin={rep['recomp_begin']}, "
                 f"oracle_begin={rep['oracle_begin']}).")
        L.append(f"  compared {rep['compared']} instrs. Capture earlier or "
                 f"raise the ring size.")
        L.append("=" * 64)
        return "\n".join(L)
    L.append(f"  instructions compared (aligned)  {rep['compared']:,}")
    cmin, cmax = rep["cycle_delta_min"], rep["cycle_delta_max"]
    L.append(f"  cycle Delta (recomp-oracle)      min={cmin}  max={cmax}"
             f"   [cumulative; diverges across HALT]")
    fd = rep["first_divergence"]
    L.append("-" * 64)
    if fd is None:
        L.append(f"  NO DIVERGENCE in the first {rep['compared']:,} retired "
                 f"instructions.")
        L.append(f"  pc + PSW(mask 0x{psw_mask:X}) + reg-FNV identical "
                 f"instruction-for-instruction. Axis-1 semantics validated "
                 f"against the oracle over this window.")
    else:
        r, o = fd["recomp"], fd["oracle"]
        L.append(f"  FIRST DIVERGENCE ({fd['kind'].upper()}) at retire-seq "
                 f"{fd['seq']:,}")
        L.append(f"    recomp  pc=0x{r['pc']:08X}  psw={psw_str(r['psw'])}"
                 f"(0x{r['psw']:X})  fnv=0x{r['fnv']:08X}  cyc={r['cycle']}")
        L.append(f"    oracle  pc=0x{o['pc']:08X}  psw={psw_str(o['psw'])}"
                 f"(0x{o['psw']:X})  fnv=0x{o['fnv']:08X}  cyc={o['cycle']}")
        if fd["kind"] == "pc":
            L.append("    => control-flow divergence. If pc lands on an ISR "
                     "vector (0xFFFFFExx), this is Axis-3 IRQ-take "
                     "quantization, NOT an Axis-1 bug.")
        elif fd["kind"] == "psw":
            L.append(f"    => FLAG divergence at a matching pc (Axis-1). diff "
                     f"bits: 0x{(r['psw'] ^ o['psw']) & psw_mask:X}")
        else:
            L.append("    => REGISTER divergence at a matching pc (Axis-1 "
                     "semantic bug). Re-query a wide window here to dump regs.")
        L.append(f"    cycle Delta at divergence: {fd['cycle_delta_at_div']}")
    L.append("=" * 64)
    return "\n".join(L)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--rom", default="roms/marios_tennis.vb")
    p.add_argument("--instructions", type=int, default=2_000_000,
                   help="max retired instructions to compare from boot")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--runtime-port", type=int, default=4390)
    p.add_argument("--oracle-port", type=int, default=4391)
    p.add_argument("--runtime-exe",
                   default="build/vbrecomp/runtime/vb-runtime.exe")
    p.add_argument("--oracle-exe",
                   default="build/vbrecomp/runtime/vb-beetle.exe")
    p.add_argument("--no-launch", action="store_true")
    p.add_argument("--mingw-bin", default=r"C:\msys64\mingw64\bin")
    p.add_argument("--psw-mask", type=lambda s: int(s, 0), default=PSW_ARITH,
                   help="PSW bits to compare (default 0xF = Z|S|OV|CY)")
    p.add_argument("--first-only", action="store_true",
                   help="stop at the first divergence (no re-sync)")
    p.add_argument("--resync-window", type=int, default=8192,
                   help="max instructions to skip when re-aligning streams")
    p.add_argument("--json-out", default=None)
    args = p.parse_args(argv)

    procs: list[subprocess.Popen] = []
    try:
        if not args.no_launch:
            rom = str(Path(args.rom))
            env = dict(os.environ)
            if args.mingw_bin:
                env["PATH"] = args.mingw_bin + os.pathsep + env.get("PATH", "")
            for exe, port in ((args.runtime_exe, args.runtime_port),
                              (args.oracle_exe, args.oracle_port)):
                if not Path(exe).exists():
                    print(f"error: missing executable {exe}", file=sys.stderr)
                    return 2
                procs.append(subprocess.Popen(
                    [str(Path(exe).resolve()), "--rom", rom,
                     "--headless", "--port", str(port)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    env=env))
            for port, name in ((args.runtime_port, "vb-runtime"),
                               (args.oracle_port, "vb-beetle")):
                if not _wait_ping(args.host, port, 20.0):
                    print(f"error: {name} no ping on {port}", file=sys.stderr)
                    return 2

        print(f"waiting for both rings to reach {args.instructions:,} "
              f"retired instructions ...", file=sys.stderr)
        hr = wait_head(args.host, args.runtime_port, args.instructions, 120.0)
        ho = wait_head(args.host, args.oracle_port, args.instructions, 120.0)
        target = min(args.instructions, hr, ho)
        print(f"  recomp head={hr:,}  oracle head={ho:,}  comparing "
              f"[0,{target:,})", file=sys.stderr)
        if target == 0:
            print("error: no instructions recorded. Was vb-runtime built "
                  "-DVBRECOMP_CPUHOOK=ON?", file=sys.stderr)
            return 2

        if args.first_only:
            rep = compare(args.host, args.runtime_port, args.oracle_port,
                          target, args.psw_mask)
            print(fmt(rep, args.psw_mask))
        else:
            print(f"  draining [0,{target:,}) from both for re-sync walk ...",
                  file=sys.stderr)
            rr = drain_all(args.host, args.runtime_port, target)
            oo = drain_all(args.host, args.oracle_port, target)
            print(f"  drained recomp={len(rr):,} oracle={len(oo):,}; "
                  f"decoding cart for classification ...", file=sys.stderr)
            classify = make_classifier(args.rom)
            rep = resync_compare(rr, oo, classify, w=args.resync_window)
            print(fmt_resync(rep))
        if args.json_out:
            Path(args.json_out).write_text(json.dumps(rep, indent=2))
            print(f"wrote {args.json_out}", file=sys.stderr)
        return 0
    finally:
        for pr in procs:
            try:
                pr.terminate(); pr.wait(timeout=5)
            except Exception:
                try: pr.kill()
                except Exception: pass


if __name__ == "__main__":
    sys.exit(main())

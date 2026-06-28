"""audio_compare.py — drift-tolerant recomp-vs-oracle audio diff.

Axis-5 (peripherals / audio) of the VB accuracy burndown. Mirrors the
psxrecomp cycle_compare.py shape: launch two independent processes, pull
their ALWAYS-ON ring buffers over TCP, and diff. Per CLAUDE.md Rule 14
the two emulators are separate processes with the same JSON wire
protocol on different ports (vb-runtime 4390, vb-beetle 4391); we query
each side's `audio_pcm` ring for the window [0, N) — a ring QUERY from
boot, never an armed capture (global always-on-ring rule).

Both sides emit interleaved S16 stereo at 44.1 kHz. Bit-exact agreement
is NOT a realistic gate for VB audio: the recomp ports Mednafen's VSU
state machine verbatim but replaces the Blip_Synth band-limited output
stage with a direct DC-centered *4 mix, and — more importantly — drives
the VSU off a coarse cycles-per-basic-block estimate rather than true
V810 instruction timing, so note onset timing and tempo drift relative
to the oracle. The metric is therefore drift-TOLERANT and layered:

  1. global alignment lag  (FFT cross-correlation)        — phase offset
  2. normalized xcorr peak (0..1) at best lag             — waveshape
  3. level ratio (dB)      (auto-calibrated RMS)          — loudness
  4. windowed envelope correlation                        — dynamics/timing
  5. onset-timing drift    (matched-onset linear fit)     — cumulative drift
  6. per-window pitch error (cents, autocorrelation f0)   — frequency fidelity

Usage (launch both, capture 10 s from boot, diff):
    python tools/audio_compare.py --rom roms/marios_tennis.vb --seconds 10

Attach to already-running processes instead of launching:
    python tools/audio_compare.py --no-launch --seconds 8

Dump the aligned streams for listening / re-analysis:
    python tools/audio_compare.py --rom roms/marios_tennis.vb --wav-out cap
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

RATE = 44100  # both sides nominal output rate (VSU_OUTPUT_HZ)


# --------------------------------------------------------------------------
# TCP wire (same line-delimited JSON as the other vb diff tools)
# --------------------------------------------------------------------------
def _send(host: str, port: int, cmd: dict, timeout: float = 10.0) -> dict:
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall((json.dumps(cmd) + "\n").encode("ascii"))
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.decode("ascii", errors="replace").strip())


def _wait_ping(host: str, port: int, deadline_s: float) -> bool:
    end = time.time() + deadline_s
    while time.time() < end:
        try:
            r = _send(host, port, {"cmd": "ping"}, timeout=2.0)
            if r.get("ok"):
                return True
        except OSError:
            time.sleep(0.1)
    return False


def drain_audio(host: str, port: int, target_frames: int,
                label: str, settle_s: float = 30.0) -> tuple[np.ndarray, dict]:
    """Pull [0, target_frames) stereo S16 from the always-on ring.

    Streams by absolute index; tolerates the producer running ahead of
    the consumer as long as we stay within the ring's resident window.
    Returns (frames[N,2] int16, meta). meta records any gap (frames lost
    because we fell behind the ring) — surfaced, never silently skipped.
    """
    chunks: list[np.ndarray] = []
    cursor = 0          # next absolute frame index we still need
    got_total = 0
    lost = 0
    stall_deadline = time.time() + settle_s
    last_progress = time.time()
    rate = RATE
    while got_total < target_frames:
        r = _send(host, port, {"cmd": "audio_pcm",
                               "start": cursor, "max": 16384})
        if not r.get("ok"):
            raise RuntimeError(f"{label}: audio_pcm failed: {r}")
        rate = int(r.get("rate", RATE))
        begin = int(r["begin"])
        returned = int(r["returned"])
        if begin > cursor:
            # The ring evicted [cursor, begin) before we read it.
            lost += begin - cursor
            cursor = begin
        if returned:
            hexs = r["hex"]
            raw = bytes.fromhex(hexs)
            arr = np.frombuffer(raw, dtype="<i2").reshape(-1, 2)
            chunks.append(arr)
            got_total += arr.shape[0]
            cursor = begin + returned
            last_progress = time.time()
        else:
            # Producer hasn't generated this far yet; wait for more.
            if time.time() - last_progress > settle_s:
                break
            time.sleep(0.02)
        if time.time() > stall_deadline and got_total == 0:
            break
    pcm = (np.concatenate(chunks, axis=0) if chunks
           else np.zeros((0, 2), dtype=np.int16))
    if pcm.shape[0] > target_frames:
        pcm = pcm[:target_frames]
    meta = {"frames": int(pcm.shape[0]), "lost_frames": int(lost),
            "rate": rate}
    return pcm, meta


# --------------------------------------------------------------------------
# Metric primitives
# --------------------------------------------------------------------------
def to_mono(x: np.ndarray) -> np.ndarray:
    return x.astype(np.float64).mean(axis=1)


def rms(x: np.ndarray) -> float:
    return float(np.sqrt(np.mean(x.astype(np.float64) ** 2))) if x.size else 0.0


def best_lag(a: np.ndarray, b: np.ndarray, max_lag: int) -> tuple[int, float]:
    """Integer lag (samples) maximizing normalized cross-correlation.

    Positive lag => b is delayed relative to a (b starts later). Returns
    (lag, normalized_peak in [-1,1]).
    """
    n = min(a.size, b.size)
    if n < 16:
        return 0, 0.0
    a = a[:n] - a[:n].mean()
    b = b[:n] - b[:n].mean()
    from scipy.signal import correlate
    full = correlate(b, a, mode="full", method="fft")
    lags = np.arange(-(n - 1), n)
    keep = np.abs(lags) <= max_lag
    full = full[keep]
    lags = lags[keep]
    denom = (np.sqrt(np.sum(a * a) * np.sum(b * b)) + 1e-12)
    ncc = full / denom
    i = int(np.argmax(ncc))
    return int(lags[i]), float(ncc[i])


def envelope(x: np.ndarray, hop: int, win: int) -> np.ndarray:
    """Short-time RMS envelope."""
    if x.size < win:
        return np.array([rms(x)])
    n = 1 + (x.size - win) // hop
    out = np.empty(n)
    xf = x.astype(np.float64)
    for i in range(n):
        seg = xf[i * hop:i * hop + win]
        out[i] = np.sqrt(np.mean(seg * seg))
    return out


def onset_times(env: np.ndarray, hop: int, rate: int,
                thresh_rel: float = 0.18) -> np.ndarray:
    """Onset times (seconds) from positive envelope flux peaks."""
    if env.size < 3:
        return np.array([])
    flux = np.diff(env)
    flux[flux < 0] = 0.0
    if flux.max() <= 0:
        return np.array([])
    thr = thresh_rel * flux.max()
    idx = []
    for i in range(1, flux.size - 1):
        if flux[i] > thr and flux[i] >= flux[i - 1] and flux[i] > flux[i + 1]:
            idx.append(i + 1)  # +1: flux[i] is between env[i] and env[i+1]
    return np.array(idx) * (hop / rate)


def local_pitch_and_drift(r: np.ndarray, o: np.ndarray, rate: int,
                          win_s: float = 0.25, search_s: float = 0.12):
    """Tempo-drift-robust comparison.

    A single global lag cannot align two streams that drift in tempo
    (note timing diverges over the clip). Instead we walk short windows,
    locally re-align each (bounded search around the running offset), and
    measure (a) per-window pitch error in cents on the aligned pair, and
    (b) the local lag track — its slope is the tempo desync in ms/s.
    Returns (signed_median_cents, median_abs_cents, frac_within_semitone,
    drift_ms_per_s, n_windows). The SIGNED median isolates a pitch-SCALE
    error (a constant clock/divider bug biases every note the same way);
    the abs spread is inflated by note mismatch when the streams have
    drifted, so a near-zero signed median with a large abs spread reads
    as "pitch scale correct, notes desynced by tempo drift."
    """
    W = int(win_s * rate)
    half = int(search_s * rate)
    if r.size < 3 * W or o.size < 3 * W:
        return float("nan"), float("nan"), float("nan"), 0
    from scipy.signal import correlate
    n = min(r.size, o.size)
    cur = 0
    cents, times, lags = [], [], []
    for k in range(0, n - 2 * W, W):
        a = r[k:k + W] - r[k:k + W].mean()
        b = o[k:k + W] - o[k:k + W].mean()
        if rms(a) < 20 or rms(b) < 20:
            continue
        cc = correlate(b, a, mode="full", method="fft")
        lg = np.arange(-(W - 1), W)
        keep = np.abs(lg - cur) <= half
        cc, lg = cc[keep], lg[keep]
        if cc.size == 0:
            continue
        lag = int(lg[int(np.argmax(cc))])
        cur = lag
        kk = k + lag
        if kk < 0 or kk + W > o.size:
            continue
        fr = estimate_f0(r[k:k + W], rate)
        fo = estimate_f0(o[kk:kk + W], rate)
        times.append(k / rate)
        lags.append(lag / rate * 1000.0)
        if fr > 0 and fo > 0:
            cents.append(1200.0 * np.log2(fr / fo))
    drift = float("nan")
    if len(times) >= 3:
        drift = float(np.polyfit(np.array(times), np.array(lags), 1)[0])
    if not cents:
        return float("nan"), float("nan"), float("nan"), drift, len(times)
    c = np.array(cents)
    ac = np.abs(c)
    return (float(np.median(c)), float(np.median(ac)),
            float((ac < 100).mean()), drift, len(cents))


def estimate_f0(seg: np.ndarray, rate: int,
                fmin: float = 60.0, fmax: float = 2000.0) -> float:
    """Autocorrelation f0 estimate with parabolic interpolation; 0 if unvoiced."""
    seg = seg.astype(np.float64)
    seg = seg - seg.mean()
    if np.sqrt(np.mean(seg * seg)) < 1.0:
        return 0.0
    ac = np.correlate(seg, seg, mode="full")[seg.size - 1:]
    if ac[0] <= 0:
        return 0.0
    lo = int(rate / fmax)
    hi = min(int(rate / fmin), ac.size - 2)
    if hi <= lo + 1:
        return 0.0
    region = ac[lo:hi]
    k = int(np.argmax(region)) + lo
    if ac[k] / ac[0] < 0.3:          # weak periodicity => treat as unvoiced
        return 0.0
    a, b, c = ac[k - 1], ac[k], ac[k + 1]
    denom = (a - 2 * b + c)
    shift = 0.5 * (a - c) / denom if denom != 0 else 0.0
    period = k + shift
    return rate / period if period > 0 else 0.0


# --------------------------------------------------------------------------
# Full comparison
# --------------------------------------------------------------------------
def compare(recomp: np.ndarray, oracle: np.ndarray, rate: int,
            max_lag_s: float = 2.5) -> dict:
    rep: dict = {}
    rmono = to_mono(recomp)
    omono = to_mono(oracle)
    rep["recomp_frames"] = int(recomp.shape[0])
    rep["oracle_frames"] = int(oracle.shape[0])
    rep["recomp_rms"] = rms(rmono)
    rep["oracle_rms"] = rms(omono)
    silent = rep["recomp_rms"] < 1.0 or rep["oracle_rms"] < 1.0
    rep["recomp_silent"] = rep["recomp_rms"] < 1.0
    rep["oracle_silent"] = rep["oracle_rms"] < 1.0

    # 1. global alignment lag (search ±max_lag_s — must exceed the
    #    boot lead-in difference between the two, which can be ~1-2 s)
    max_lag = int(max_lag_s * rate)
    lag, ncc = best_lag(rmono, omono, max_lag)
    rep["lag_samples"] = lag
    rep["lag_ms"] = 1000.0 * lag / rate
    rep["xcorr_ncc"] = ncc

    # align by lag for the remaining metrics
    if lag >= 0:
        ra, oa = rmono[:rmono.size - lag] if lag else rmono, omono[lag:]
        rs, os_ = (recomp[:recomp.shape[0] - lag] if lag else recomp,
                   oracle[lag:])
    else:
        ra, oa = rmono[-lag:], omono[:omono.size + lag]
        rs, os_ = recomp[-lag:], oracle[:oracle.shape[0] + lag]
    n = min(ra.size, oa.size)
    ra, oa = ra[:n], oa[:n]
    rs, os_ = rs[:n], os_[:n]

    # 2. level ratio (auto-calibrated loudness offset)
    rr, orr = rms(ra), rms(oa)
    ratio = (orr / rr) if rr > 0 else 0.0
    rep["level_ratio_oracle_over_recomp"] = ratio
    rep["level_offset_db"] = (20.0 * np.log10(ratio)) if ratio > 0 else float("nan")

    # 3. windowed envelope correlation (10 ms hop / 25 ms win)
    hop = int(0.010 * rate)
    win = int(0.025 * rate)
    er = envelope(ra, hop, win)
    eo = envelope(oa, hop, win)
    m = min(er.size, eo.size)
    if m > 4 and er[:m].std() > 0 and eo[:m].std() > 0:
        rep["envelope_corr"] = float(np.corrcoef(er[:m], eo[:m])[0, 1])
    else:
        rep["envelope_corr"] = float("nan")

    # 4. onset-timing drift (matched onsets -> linear fit of delta vs time)
    o_r = onset_times(er, hop, rate)
    o_o = onset_times(eo, hop, rate)
    deltas = []
    times = []
    for t in o_o:
        if o_r.size == 0:
            break
        j = int(np.argmin(np.abs(o_r - t)))
        d = o_r[j] - t
        if abs(d) < 0.060:           # within 60 ms = same onset
            deltas.append(d)
            times.append(t)
    rep["onsets_oracle"] = int(o_o.size)
    rep["onsets_recomp"] = int(o_r.size)
    rep["onsets_matched"] = len(deltas)
    if len(deltas) >= 3:
        deltas = np.array(deltas)
        times = np.array(times)
        rep["onset_delta_mean_ms"] = float(1000 * deltas.mean())
        rep["onset_delta_std_ms"] = float(1000 * deltas.std())
        slope, _ = np.polyfit(times, deltas, 1)
        rep["onset_drift_ms_per_s"] = float(1000 * slope)
    else:
        rep["onset_delta_mean_ms"] = float("nan")
        rep["onset_delta_std_ms"] = float("nan")
        rep["onset_drift_ms_per_s"] = float("nan")

    # 5. per-window pitch error in cents (voiced windows only)
    pwin = int(0.050 * rate)
    cents = []
    voiced = 0
    for i in range(0, n - pwin, pwin):
        fr = estimate_f0(ra[i:i + pwin], rate)
        fo = estimate_f0(oa[i:i + pwin], rate)
        if fr > 0 and fo > 0:
            voiced += 1
            cents.append(1200.0 * np.log2(fr / fo))
    rep["pitch_windows_voiced"] = voiced
    if cents:
        cents = np.array(cents)
        rep["pitch_cents_median_abs"] = float(np.median(np.abs(cents)))
        rep["pitch_cents_p90_abs"] = float(np.percentile(np.abs(cents), 90))
    else:
        rep["pitch_cents_median_abs"] = float("nan")
        rep["pitch_cents_p90_abs"] = float("nan")

    # 5b. drift-robust local pitch + tempo desync (a single global lag
    #     cannot align tempo-drifting streams; walk + locally re-align)
    lp_signed, lp_abs, lp_within, drift_ms_s, lp_n = \
        local_pitch_and_drift(ra, oa, rate)
    rep["local_pitch_cents_median_signed"] = lp_signed
    rep["local_pitch_cents_median_abs"] = lp_abs
    rep["local_pitch_within_semitone"] = lp_within
    rep["tempo_drift_ms_per_s"] = drift_ms_s
    rep["local_pitch_windows"] = lp_n

    rep["_silent"] = silent
    return rep


def verdict(rep: dict) -> tuple[str, list[str]]:
    notes = []
    if rep.get("recomp_silent"):
        notes.append("RECOMP PRODUCED SILENCE — no VSU output captured.")
    if rep.get("oracle_silent"):
        notes.append("ORACLE PRODUCED SILENCE — oracle audio tap not firing.")
    if rep.get("_silent"):
        return "INCONCLUSIVE", notes
    ncc = rep.get("xcorr_ncc", 0.0)
    env = rep.get("envelope_corr", float("nan"))
    # Pitch (drift-robust, locally aligned) is the primary content gate
    # for VB: the output stage differs from the oracle by design
    # (DC-center + <<2 vs Blip band-limiting), so raw waveshape NCC
    # understates a real note-for-note match.
    bias = rep.get("local_pitch_cents_median_signed", float("nan"))
    absb = abs(bias) if bias == bias else float("nan")
    drift = rep.get("tempo_drift_ms_per_s", float("nan"))
    scale_ok = absb == absb and absb < 60      # pitch-scale (per-note bias) correct
    if scale_ok and ncc >= 0.6:
        v = "STRONG MATCH"
    elif scale_ok and (drift == drift and abs(drift) > 5.0):
        v = "PITCH-SCALE CORRECT — residual tempo drift"
    elif scale_ok:
        v = "PITCH MATCH (note-accurate)"
    elif absb == absb and absb < 300:
        v = "NEAR — pitch scale close"
    elif absb == absb and absb < 1200:
        v = "PARTIAL — pitch scale off"
    else:
        v = "WEAK / DIVERGENT (gross pitch error)"
    if drift == drift and abs(drift) > 5.0:
        notes.append(f"Tempo drift {drift:+.1f} ms/s — note sequencing "
                     f"desyncs over the clip (Axis 2: coarse cycle "
                     f"estimate driving music timing).")
    return v, notes


def fmt(rep: dict) -> str:
    def g(k, f="{:.4f}"):
        v = rep.get(k)
        if v is None:
            return "n/a"
        if isinstance(v, float) and v != v:
            return "n/a"
        return f.format(v) if isinstance(v, float) else str(v)
    L = []
    L.append("=" * 64)
    L.append("  VB AUDIO ACCURACY — recomp (vb-runtime) vs oracle (vb-beetle)")
    L.append("=" * 64)
    L.append(f"  captured frames     recomp={g('recomp_frames')}  "
             f"oracle={g('oracle_frames')}  @ {RATE} Hz stereo")
    L.append(f"  RMS                 recomp={g('recomp_rms','{:.1f}')}  "
             f"oracle={g('oracle_rms','{:.1f}')}")
    L.append("-" * 64)
    L.append(f"  1. alignment lag    {g('lag_samples')} samp "
             f"({g('lag_ms','{:+.2f}')} ms)")
    L.append(f"  2. xcorr peak (NCC) {g('xcorr_ncc','{:.4f}')}   "
             f"[1.0 = identical waveshape]")
    L.append(f"  3. level offset     {g('level_offset_db','{:+.2f}')} dB "
             f"(ratio {g('level_ratio_oracle_over_recomp','{:.3f}')} oracle/recomp)")
    L.append(f"  4. envelope corr    {g('envelope_corr','{:.4f}')}   "
             f"[dynamics/timing similarity]")
    L.append(f"  5. onset timing     matched {g('onsets_matched')}/"
             f"{g('onsets_oracle')}  mean {g('onset_delta_mean_ms','{:+.2f}')} ms"
             f"  std {g('onset_delta_std_ms','{:.2f}')} ms"
             f"  drift {g('onset_drift_ms_per_s','{:+.3f}')} ms/s")
    L.append(f"  6. pitch error      median |{g('pitch_cents_median_abs','{:.1f}')}| "
             f"cents  p90 {g('pitch_cents_p90_abs','{:.1f}')} cents  "
             f"(voiced {g('pitch_windows_voiced')} win)")
    L.append(f"  6b.LOCAL pitch      bias {g('local_pitch_cents_median_signed','{:+.0f}')} cents "
             f"(scale error)  |med| {g('local_pitch_cents_median_abs','{:.0f}')}  "
             f"(drift-aligned, {g('local_pitch_windows')} win)")
    L.append(f"  7. tempo drift      {g('tempo_drift_ms_per_s','{:+.1f}')} ms/s "
             f"(note-sequencing desync)")
    L.append("-" * 64)
    v, notes = verdict(rep)
    L.append(f"  VERDICT: {v}")
    for nnote in notes:
        L.append(f"    ! {nnote}")
    L.append("=" * 64)
    return "\n".join(L)


# --------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--rom", default="roms/marios_tennis.vb")
    p.add_argument("--seconds", type=float, default=10.0,
                   help="audio duration to capture from boot (s)")
    p.add_argument("--max-lag-s", type=float, default=2.5,
                   help="cross-correlation alignment search half-window (s)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--runtime-port", type=int, default=4390)
    p.add_argument("--oracle-port", type=int, default=4391)
    p.add_argument("--runtime-exe",
                   default="build/vbrecomp/runtime/vb-runtime.exe")
    p.add_argument("--oracle-exe",
                   default="build/vbrecomp/runtime/vb-beetle.exe")
    p.add_argument("--no-launch", action="store_true",
                   help="attach to already-running processes")
    p.add_argument("--mingw-bin", default=r"C:\msys64\mingw64\bin",
                   help="prepended to child PATH so SDL2/libstdc++ DLLs resolve")
    p.add_argument("--wav-out", default="",
                   help="basename to dump aligned recomp/oracle WAVs + npz")
    p.add_argument("--json-out", default="",
                   help="write the metric report as JSON")
    args = p.parse_args(argv)

    target = int(args.seconds * RATE)
    procs: list[subprocess.Popen] = []
    try:
        if not args.no_launch:
            rom = str(Path(args.rom))
            child_env = dict(os.environ)
            if args.mingw_bin:
                child_env["PATH"] = args.mingw_bin + os.pathsep + child_env.get("PATH", "")
            for exe, port in ((args.runtime_exe, args.runtime_port),
                              (args.oracle_exe, args.oracle_port)):
                exe_path = Path(exe)
                if not exe_path.exists():
                    print(f"error: missing executable {exe}", file=sys.stderr)
                    return 2
                procs.append(subprocess.Popen(
                    [str(exe_path.resolve()), "--rom", rom,
                     "--headless", "--port", str(port)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    env=child_env))
            for port, name in ((args.runtime_port, "vb-runtime"),
                               (args.oracle_port, "vb-beetle")):
                if not _wait_ping(args.host, port, 20.0):
                    print(f"error: {name} did not answer ping on {port}",
                          file=sys.stderr)
                    return 2

        print(f"draining {args.seconds:.1f}s ({target} frames) "
              f"from boot on both rings ...", file=sys.stderr)
        recomp, rmeta = drain_audio(args.host, args.runtime_port, target,
                                    "vb-runtime")
        oracle, ometa = drain_audio(args.host, args.oracle_port, target,
                                    "vb-beetle")
        print(f"  recomp: {rmeta}", file=sys.stderr)
        print(f"  oracle: {ometa}", file=sys.stderr)

        rep = compare(recomp, oracle, RATE, max_lag_s=args.max_lag_s)
        rep["_meta"] = {"recomp": rmeta, "oracle": ometa,
                        "rom": args.rom, "seconds": args.seconds}
        print(fmt(rep))

        if args.wav_out:
            import wave
            for name, pcm in (("recomp", recomp), ("oracle", oracle)):
                wp = f"{args.wav_out}_{name}.wav"
                with wave.open(wp, "wb") as w:
                    w.setnchannels(2)
                    w.setsampwidth(2)
                    w.setframerate(RATE)
                    w.writeframes(pcm.astype("<i2").tobytes())
                print(f"wrote {wp}", file=sys.stderr)
            np.savez(f"{args.wav_out}.npz", recomp=recomp, oracle=oracle)
            print(f"wrote {args.wav_out}.npz", file=sys.stderr)
        if args.json_out:
            Path(args.json_out).write_text(json.dumps(rep, indent=2))
            print(f"wrote {args.json_out}", file=sys.stderr)
        return 0
    finally:
        for pr in procs:
            try:
                pr.terminate()
                pr.wait(timeout=5)
            except Exception:
                try:
                    pr.kill()
                except Exception:
                    pass


if __name__ == "__main__":
    raise SystemExit(main())

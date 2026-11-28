#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""The engine ECU on the board, at constant crank speeds (Level 2, v0.4 step 6).

No plant: the Tang Nano 20K's bridge turns the Level 2 wheel at a fixed speed
and captures the six injector lines; the NUCLEO-G431KB runs the reference
engine ECU (platforms/stm32g4/ecu/). Two parts:

1. Injections, with oc-stm32g4-ecu-fixedfuel (the width fixed at 20 % fuel,
   520 us, because at a constant speed the governor would stop injecting).
   For each speed (default 200, 750, 1500 and 2400 rpm, 30 s each, after 2 s
   of warm-up), the bridge is loaded fresh (its accumulator is the angle
   only from a fresh bitstream) and open-crank-ecu-check runs the hard loop,
   collects the captures and judges them: injections per channel, the start
   of each against TDC - 10 degrees (within 0.5, p99 <= 0.1), the width
   (520 us x 1.0027 +- 1 us) and lost captures. This script adds the ECU's own
   view from USART2: sync FULL in every row of the judged window, and no
   sync loss, missed arming, refused arming or capture overrun during it.
2. EEC1, with the reference build oc-stm32g4-ecu: for each speed (10 s), the
   frames from address 0x00 on the CAN interface (candump, kernel times):
   one every 10 ms, and the engine speed about rpm x 1.0027 (the ECU reads
   speed with its HSI, F-22).

At the end the NUCLEO runs oc-stm32g4-ecu, the reference build.

Usage:
  tools/board/ecucheck.py --check <open-crank build>/open-crank-ecu-check \\
      --bitstream <open-tick>/build-gateware/bridge.fs [--rpm 200,750,1500,2400]
      [--seconds 30] [--eec1-seconds 10] [--iface can0] [--no-eec1] [--out DIR]

Needs openFPGALoader on PATH (open-tick: eval "$(tools/fpga_toolchain.sh)"),
candump (can-utils) for the EEC1 part, the CAN interface up, and the tools of
tools/board/setup.sh. Python standard library only. Exit 0 every check ok,
1 a check failed, 2 the bench is not ready.
"""
import argparse
import hashlib
import os
import re
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True  # no __pycache__ next to the tools
sys.path.insert(0, HERE)
from wirecheck import SerialLines, one, run  # noqa: E402

WARMUP_S = 2.0
HSI_BIAS = 0.0027
EEC1_ID = 0x0CF00400   # EEC1 (PGN 61444) from the engine, address 0x00, priority 3
EEC1_PERIOD_MS = 10.0
EEC1_PERIOD_TOL = 0.01  # the mean period, against the host's clock (the HSI makes it 0.27 % long)
EEC1_GAP_MAX_MS = 20.0  # no gap of two periods
EEC1_RPM_TOL = 0.002    # the speed ratio within 1.0027 +- 0.2 %
COLS = ("ms,sync,rpm,target_rpm,fuel_pct,width_us,sync_losses,gaps,injections,arms,missed,conflicts,"
        "teeth,cam,stops,overruns,isr_cycles_max,step_cycles_max,tx_dropped").split(",")


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def rows_between(reader, t_from, t_to):
    """The ECU's CSV rows received in (t_from, t_to], as dicts (F-54: only the run's window)."""
    out = []
    for t, line in reader.lines:
        if not (t_from < t <= t_to):
            continue
        f = line.split(",")
        if len(f) != len(COLS) or not f[0].isdigit():
            continue
        r = dict(zip(COLS, f))
        for k in COLS:
            if k != "sync":
                r[k] = float(r[k])
        out.append(r)
    return out


def ecu_view(rows):
    """Sync, losses and the ECU's counters over the judged window; (ok, text, worst cycles)."""
    if len(rows) < 5:
        return False, f"only {len(rows)} USART2 rows in the window", (0, 0)
    not_full = sum(1 for r in rows if r["sync"] != "FULL")
    d = {k: rows[-1][k] - rows[0][k] for k in ("sync_losses", "missed", "conflicts", "overruns", "injections")}
    ok = not_full == 0 and d["sync_losses"] == 0 and d["missed"] == 0 and d["conflicts"] == 0 and d["overruns"] == 0
    rpm = sorted(r["rpm"] for r in rows)[len(rows) // 2]
    text = (f"{len(rows)} rows, {not_full} not FULL; ECU reads {rpm:.1f} rpm (median); during the window "
            f"{d['injections']:.0f} injections fired, {d['sync_losses']:.0f} sync losses, {d['missed']:.0f} missed, "
            f"{d['conflicts']:.0f} refused, {d['overruns']:.0f} overruns")
    worst = (int(max(r["isr_cycles_max"] for r in rows)), int(max(r["step_cycles_max"] for r in rows)))
    return ok, text, worst


def load_bridge(bitstream):
    run(["openFPGALoader", "-b", "tangnano20k", bitstream], "load the bridge into the Tang's SRAM (fresh)")
    time.sleep(1.0)


def injections(a, tang, reader, rpm, out, commit, bmd5):
    """Part 1 at one speed: (ok, summary lines, worst cycles)."""
    tag = f"{rpm:.0f}rpm"
    for attempt in (1, 2):
        load_bridge(a.bitstream)
        print(f"== {rpm:.0f} rpm, {a.seconds:.0f} s (attempt {attempt})", flush=True)
        t0 = time.monotonic()
        cmd = [a.check, "--dev", tang, "--rpm", str(rpm), "--seconds", str(a.seconds), "--warmup", str(WARMUP_S),
               "--captures", os.path.join(out, f"captures_{tag}.csv"),
               "--jitter", os.path.join(out, f"jitter_{tag}.csv"),
               "--budget", os.path.join(out, f"hard_budget_{tag}.csv"),
               "--bitstream-md5", bmd5, "--commit", commit]
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        t1 = time.monotonic()
        with open(os.path.join(out, f"check_{tag}.txt" if attempt == 1 else f"check_{tag}.attempt2.txt"), "w") as f:
            f.write(r.stdout)
        if r.returncode != 3:
            break
        print(f"   the hard run was not ok (exit 3); {'retrying once' if attempt == 1 else 'giving up'}", flush=True)
    print(r.stdout, end="", flush=True)
    time.sleep(0.5)
    m = re.search(r"step 0 at monotonic (\d+) ns", r.stdout)
    if not m:
        return False, r.returncode, "no step 0 time from open-crank-ecu-check", (0, 0), t1 - t0
    s0 = int(m.group(1)) / 1e9
    rows = rows_between(reader, s0 + WARMUP_S, s0 + WARMUP_S + a.seconds)
    v_ok, v_text, worst = ecu_view(rows)
    if attempt == 2:
        v_text += "; the first hard run was not ok, this is the retry (check_" + tag + ".txt has the first)"
    print(f"  {'ok  ' if v_ok else 'FAIL'}  USART2: {v_text}", flush=True)
    return r.returncode == 0 and v_ok, r.returncode, v_text, worst, t1 - t0


def eec1(a, reader, rpm, out):
    """Part 2 at one speed: (ok, text)."""
    tag = f"{rpm:.0f}rpm"
    load_bridge(a.bitstream)
    log = os.path.join(out, f"eec1_{tag}.candump.log")
    with open(log, "w") as f:
        dump = subprocess.Popen(["candump", "-L", f"{a.iface},{EEC1_ID:08X}:1FFFFFFF"], stdout=f,
                                stderr=subprocess.DEVNULL)
    time.sleep(0.3)
    secs = a.eec1_seconds + WARMUP_S
    print(f"== EEC1 at {rpm:.0f} rpm, {secs:.0f} s", flush=True)
    r = subprocess.run([a.check, "--dev", a.tang, "--rpm", str(rpm), "--seconds", str(a.eec1_seconds),
                        "--warmup", str(WARMUP_S)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(0.3)
    dump.send_signal(signal.SIGINT)
    dump.wait()
    with open(os.path.join(out, f"eec1_{tag}_check.txt"), "w") as f:
        f.write(r.stdout)
    frames = []
    with open(log) as f:
        for line in f:
            m = re.match(r"\((\d+\.\d+)\) \S+ ([0-9A-F]{8})#([0-9A-F]{16})", line)
            if m and int(m.group(2), 16) == EEC1_ID:
                d = bytes.fromhex(m.group(3))
                frames.append((float(m.group(1)), (d[3] | d[4] << 8) * 0.125))
    if len(frames) < 100:
        return False, f"only {len(frames)} EEC1 frames"
    # the speed is judged once the crank has turned WARMUP_S: the frames whose speed has settled
    t_first = next((t for t, s in frames if s > 0.0), None)
    if t_first is None:
        return False, "every EEC1 has speed 0"
    judged = [(t, s) for t, s in frames if t_first + WARMUP_S <= t <= t_first + WARMUP_S + a.eec1_seconds - 0.5]
    gaps = [(frames[i + 1][0] - frames[i][0]) * 1e3 for i in range(len(frames) - 1)]
    mean = sum(gaps) / len(gaps)
    want = rpm * (1.0 + HSI_BIAS)
    ratios = [s / rpm for _, s in judged]
    bad = [s for _, s in judged if abs(s / want - 1.0) > EEC1_RPM_TOL]
    ok = (abs(mean / EEC1_PERIOD_MS - 1.0) <= EEC1_PERIOD_TOL and max(gaps) <= EEC1_GAP_MAX_MS and judged
          and not bad and r.returncode in (0, 1))  # 1: the governed widths are not the fixed ones; the run was ok
    text = (f"{len(frames)} frames, period mean {mean:.4f} ms, max {max(gaps):.2f} ms; speed / rpm over "
            f"{len(judged)} frames: min {min(ratios):.5f}, max {max(ratios):.5f} (want {1 + HSI_BIAS:.4f} "
            f"+- {EEC1_RPM_TOL:.3f}), {len(bad)} outside; hard run exit {r.returncode}")
    return ok, text


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", required=True, help="open-crank-ecu-check")
    ap.add_argument("--bitstream", required=True, help="open-tick's bridge.fs, loaded into the Tang's SRAM")
    ap.add_argument("--rpm", default="200,750,1500,2400")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--eec1-seconds", type=float, default=10.0)
    ap.add_argument("--iface", default="can0")
    ap.add_argument("--no-eec1", action="store_true", help="injections only; the NUCLEO keeps the fixed-fuel build")
    ap.add_argument("--out", default="ecucheck-out")
    a = ap.parse_args()
    speeds = [float(x) for x in a.rpm.split(",") if x.strip()]
    if not os.access(a.check, os.X_OK) or not os.path.isfile(a.bitstream):
        print("ecucheck: --check must be an executable and --bitstream a file", file=sys.stderr)
        return 2
    if not a.no_eec1:
        if subprocess.run(["which", "candump"], stdout=subprocess.DEVNULL).returncode != 0:
            print("ecucheck: candump not found (can-utils); or --no-eec1", file=sys.stderr)
            return 2
        if not os.path.exists(f"/sys/class/net/{a.iface}"):
            print(f"ecucheck: no {a.iface}; bring it up, or --no-eec1", file=sys.stderr)
            return 2
    os.makedirs(a.out, exist_ok=True)
    vcp = one("/dev/serial/by-id/*STLINK*-if02", "ST-LINK virtual COM port")
    a.tang = one("/dev/serial/by-id/usb-SIPEED_USB_Debugger_*-if01-port0", "Tang Nano 20K UART")
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], stdout=subprocess.PIPE,
                            text=True).stdout.strip()
    bmd5 = md5(a.bitstream)
    usb = os.path.realpath(f"/sys/class/tty/{os.path.basename(os.path.realpath(a.tang))}/device")
    with open(os.path.join(a.out, "meta.txt"), "w") as f:
        f.write(f"commit={commit}\nbitstream_md5={bmd5}\ntang_usb={usb}\nhost={os.uname().nodename}\n"
                f"speeds={a.rpm}\nseconds={a.seconds}\nwarmup={WARMUP_S}\n")

    run([os.path.join(HERE, "flash.sh"), "ecu-fixedfuel"], "build and program oc-stm32g4-ecu-fixedfuel")
    reader = SerialLines(vcp)
    reader.start()
    results = []
    table = []
    for rpm in speeds:
        ok, rc, vtext, worst, _ = injections(a, a.tang, reader, rpm, a.out, commit, bmd5)
        results.append((f"injections {rpm:.0f} rpm", ok, vtext))
        table.append((rpm, worst))
    worst_all = (max(w[0] for _, w in table), max(w[1] for _, w in table)) if table else (0, 0)

    if not a.no_eec1:
        run([os.path.join(HERE, "flash.sh"), "ecu"], "build and program oc-stm32g4-ecu (the reference)")
        for rpm in speeds:
            ok, text = eec1(a, reader, rpm, a.out)
            print(f"  {'ok  ' if ok else 'FAIL'}  EEC1 at {rpm:.0f} rpm: {text}", flush=True)
            results.append((f"EEC1 {rpm:.0f} rpm", ok, text))
    reader.stop = True
    reader.join()
    with open(os.path.join(a.out, "nucleo.log"), "w") as f:
        for t, x in reader.lines:
            f.write(f"{t:.3f} {x}\n")

    print()
    lines = []
    for name, ok, text in results:
        lines.append(f"{'ok  ' if ok else 'FAIL'}  {name}: {text}")
    lines.append(f"worst cycles seen on USART2 (fixed-fuel build): tooth interrupt {worst_all[0]} "
                 f"({worst_all[0] / 170.0:.1f} us), ecu_step {worst_all[1]} ({worst_all[1] / 170.0:.1f} us)")
    failed = [n for n, ok, _ in results if not ok]
    lines.append("ecucheck: " + ("every check ok" if not failed else f"FAILED: {', '.join(failed)}"))
    lines.append("the NUCLEO now runs " + ("oc-stm32g4-ecu-fixedfuel (--no-eec1): program ecu"
                                           if a.no_eec1 else "oc-stm32g4-ecu"))
    with open(os.path.join(a.out, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the Level 2 harness, wire by wire (platforms/stm32g4/WIRING-L2.md).

The NUCLEO-G431KB runs oc-stm32g4-wirecheck: it counts the crank and cam edges
it receives and, every 100 ms, sends a pulse on each injector line, injector k
high for k x 100 us. The Tang Nano 20K runs open-tick's bridge: open-tick's
`otk-hard-probe wires` makes it turn a 60-2 crank and a cam at a fixed speed
and captures the six injector inputs.

Each line is judged on its own, so a harness wired in part reports what is
there:
  crank   the NUCLEO sees 58 teeth and one gap per revolution at the set speed
  cam     the NUCLEO sees one edge per 720 degrees
  inj1-6  the bridge's channel k captures pulses of k x 100 us, about ten a
          second; another width means the wire is crossed with that injector

Usage:
  tools/board/wirecheck.py --probe <open-tick build>/otk-hard-probe \\
      --bitstream <open-tick>/build-gateware/bridge.fs [--seconds 5] [--rpm 1500]
      [--lines crank,cam,inj1,...] [--no-flash]

Needs openFPGALoader on PATH (open-tick: eval "$(tools/fpga_toolchain.sh)"),
and the tools of tools/board/setup.sh for the NUCLEO. Python standard library
only. Exit 0 every checked line ok, 1 a line failed, 2 the bench is not ready.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import termios
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Injector k (1-6): its NUCLEO pin and Tang pin, as WIRING-L2.md.
INJ = {
    1: ("D9 (PA8)", 27),
    2: ("D1 (PA9)", 28),
    3: ("D0 (PA10)", 25),
    4: ("D12 (PB4)", 26),
    5: ("D11 (PB5)", 29),
    6: ("D6 (PB6)", 30),
}
TEETH = 58  # a 60-2 wheel
PULSE_UNIT_US = 100.0
WIDTH_TOL_US = 20.0
BURSTS_PER_S = 10


def one(pattern, what):
    found = sorted(glob.glob(pattern))
    if not found:
        sys.exit(f"wirecheck: no {what} ({pattern})")
    return found[0]


class SerialLines(threading.Thread):
    """Reads lines from a serial port at 115200 baud until stopped."""

    def __init__(self, path):
        super().__init__(daemon=True)
        self.fd = os.open(path, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        attr = termios.tcgetattr(self.fd)
        attr[0] = 0  # iflag
        attr[1] = 0  # oflag
        attr[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attr[3] = 0  # lflag: raw
        attr[4] = attr[5] = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW, attr)
        termios.tcflush(self.fd, termios.TCIFLUSH)
        self.lines = []
        self.stop = False

    def run(self):
        buf = b""
        while not self.stop:
            try:
                chunk = os.read(self.fd, 256)
            except BlockingIOError:
                chunk = b""
            if not chunk:
                time.sleep(0.01)
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.lines.append((time.monotonic(), line.decode(errors="replace").strip()))
        os.close(self.fd)


def run(cmd, what):
    print(f"== {what}", flush=True)
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        print(r.stdout)
        sys.exit(f"wirecheck: {what} failed (exit {r.returncode})")
    return r.stdout


def median(v):
    v = sorted(v)
    return v[len(v) // 2] if v else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--probe", required=True, help="open-tick's otk-hard-probe")
    ap.add_argument("--bitstream", required=True, help="open-tick's bridge.fs, loaded into the Tang's SRAM")
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--rpm", type=float, default=1500.0)
    ap.add_argument("--lines", default="crank,cam,inj1,inj2,inj3,inj4,inj5,inj6",
                    help="the lines to judge, comma-separated (default all)")
    ap.add_argument("--no-flash", action="store_true", help="the NUCLEO already runs oc-stm32g4-wirecheck")
    a = ap.parse_args()
    lines = [x.strip() for x in a.lines.split(",") if x.strip()]
    known = {"crank", "cam"} | {f"inj{k}" for k in INJ}
    bad = [x for x in lines if x not in known]
    if bad:
        sys.exit(f"wirecheck: unknown line(s) {', '.join(bad)}; known: {', '.join(sorted(known))}")
    if not os.access(a.probe, os.X_OK) or not os.path.isfile(a.bitstream):
        print("wirecheck: --probe must be an executable and --bitstream a file", file=sys.stderr)
        return 2

    vcp = one("/dev/serial/by-id/*STLINK*-if02", "ST-LINK virtual COM port")
    tang = one("/dev/serial/by-id/usb-SIPEED_USB_Debugger_*-if01-port0", "Tang Nano 20K UART")

    if not a.no_flash:
        run([os.path.join(HERE, "flash.sh"), "wirecheck"], "build and program oc-stm32g4-wirecheck")
    run(["openFPGALoader", "-b", "tangnano20k", a.bitstream], "load the bridge into the Tang's SRAM")
    time.sleep(1.0)

    reader = SerialLines(vcp)
    reader.start()
    print(f"== the bridge turns the crank at {a.rpm:.0f} rpm for {a.seconds:.0f} s", flush=True)
    t0 = time.monotonic()
    probe = subprocess.run([a.probe, "--dev", tang, "--seconds", str(a.seconds), "--rpm", str(a.rpm), "wires"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t1 = time.monotonic()
    time.sleep(1.2)  # the NUCLEO's report for the last second
    reader.stop = True
    reader.join()
    if probe.returncode != 0:
        print(probe.stdout)
        print(f"wirecheck: otk-hard-probe failed (exit {probe.returncode})", file=sys.stderr)
        return 2

    # the bridge's captures, per channel
    caps = {}
    for m in re.finditer(r"capture (\d+): (\d+) pulses(?:, width us min ([\d.]+) mean ([\d.]+) max ([\d.]+))?",
                         probe.stdout):
        caps[int(m.group(1)) + 1] = (int(m.group(2)), float(m.group(4) or 0.0))
    lost = re.search(r"captures lost \(bridge overflow\): (\d+)", probe.stdout)

    # the NUCLEO's reports over whole seconds inside the run
    rep = []
    for t, line in reader.lines:
        m = re.match(r"wirecheck s=\d+ crank=(\d+) cam=(\d+) gaps=(\d+) rpm=(\d+) ovr=(\d+) bursts=(\d+)", line)
        if m and t0 + 1.0 < t <= t1:
            rep.append(tuple(int(x) for x in m.groups()))
    if not reader.lines:
        print("wirecheck: nothing from the NUCLEO's virtual COM port: is oc-stm32g4-wirecheck running?",
              file=sys.stderr)
        return 2

    results = []
    if "crank" in lines or "cam" in lines:
        if len(rep) < 2:
            print(f"wirecheck: only {len(rep)} NUCLEO report(s) inside the run; try --seconds 5 or more",
                  file=sys.stderr)
            return 2
        crank = median([r[0] for r in rep])
        cam = median([r[1] for r in rep])
        gaps = median([r[2] for r in rep])
        rpm = median([r[3] for r in rep])
        ovr = sum(r[4] for r in rep)
        rev_s = a.rpm / 60.0
        if "crank" in lines:
            ok = (abs(crank - TEETH * rev_s) <= 0.03 * TEETH * rev_s + 2 and abs(gaps - rev_s) <= 1.5
                  and abs(rpm - a.rpm) <= 0.01 * a.rpm and ovr == 0)
            results.append(("crank", "Tang 73 -> A0 (PA0)", ok,
                            f"{crank} teeth/s (expected {TEETH * rev_s:.0f}), {gaps} gaps/s ({rev_s:.1f}), "
                            f"{rpm} rpm, {ovr} overruns"))
        if "cam" in lines:
            ok = abs(cam - rev_s / 2.0) <= 1.0
            results.append(("cam", "Tang 74 -> A1 (PA1)", ok, f"{cam} edges/s (expected {rev_s / 2.0:.1f})"))

    for k, (nucleo, tang_pin) in INJ.items():
        name = f"inj{k}"
        if name not in lines:
            continue
        n, width = caps.get(k, (0, 0.0))
        expect = k * PULSE_UNIT_US
        rate = n / a.seconds
        if n == 0:
            ok, why = False, "no pulse captured: open wire, or not wired"
        elif abs(width - expect) <= WIDTH_TOL_US and abs(rate - BURSTS_PER_S) <= 0.2 * BURSTS_PER_S:
            ok, why = True, f"{n} pulses ({rate:.1f}/s), mean width {width:.1f} us (expected {expect:.0f})"
        else:
            other = round(width / PULSE_UNIT_US)
            hint = f": crossed with injector {other}'s wire ({INJ[other][0]})" if other in INJ and other != k else ""
            ok, why = False, f"{n} pulses ({rate:.1f}/s), mean width {width:.1f} us, expected {expect:.0f}{hint}"
        results.append((name, f"{nucleo} -> Tang {tang_pin}", ok, why))

    print()
    for name, path, ok, why in results:
        print(f"{'ok  ' if ok else 'FAIL'}  {name:<5} {path:<22} {why}")
    if lost and int(lost.group(1)) != 0:
        print(f"note  the bridge lost {lost.group(1)} captures (overflow)")
    failed = [r[0] for r in results if not r[2]]
    print()
    print("wirecheck: " + ("every checked line ok" if not failed else f"FAILED: {', '.join(failed)}"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

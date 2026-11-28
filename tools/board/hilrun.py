#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Level 2 in a closed loop on the bench: the plant against the board's engine ECU (v0.4).

The NUCLEO-G431KB runs the reference engine ECU (oc-stm32g4-ecu); the Tang
Nano 20K runs open-tick's bridge; the host runs open-crank-hil: the Level 2
plant, the hoist pump and the reference machine controller, one step per
1 ms through open-tick's hard loop. The bridge turns the crank and cam from
the plant's angle and captures the six injectors; TSC1 and EEC1 go over the
CAN interface.

For each scenario (default: every scenario of scenarios/l2 the board can
play), this script:
  - loads the bridge into the Tang's SRAM, fresh: its phase accumulator is the
    plant's angle only from a freshly loaded bitstream;
  - resets the NUCLEO, so the ECU starts from rest (no latched trip);
  - runs open-crank-hil with the scenario's criteria, the fidelity check
    (sim/l2/fidelity.csv against the bridge's), and writes the trace, the
    frame log, the verdicts, the jitter budget and open-tick's budget of the
    hard run;
  - keeps the ECU's USART2 lines received during the run next to them.
A hard run that is not ok (exit 3) is retried once; both outputs are kept.
The BL616 on the Tang resets its USB now and then (about every 15 min of
running on the reference bench, 2026-11-28): such a run ends aborted with
abort=io in its budget, and the next load of the bridge is retried.

scenarios/l2/crank_tooth_lost is left out: it removes crank teeth in the
in-memory edge model, and the bridge has no such fault.

Usage:
  tools/board/hilrun.py --hil build/open-crank-hil \\
      --bitstream <open-tick>/build-gateware/bridge.fs [--scenarios a,b,...]
      [--bridge-fidelity external/open-tick/gateware/tang_nano_20k/fidelity.csv]
      [--iface can0] [--no-flash] [--out DIR]

Needs openFPGALoader on PATH (open-tick: eval "$(tools/fpga_toolchain.sh)"),
the CAN interface up, and the tools of tools/board/setup.sh. Python standard
library only. Exit 0 every scenario passed, 1 one did not, 2 the bench is not
ready. The NUCLEO is left running oc-stm32g4-ecu.
"""
import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.dont_write_bytecode = True  # no __pycache__ next to the tools
sys.path.insert(0, HERE)
from wirecheck import SerialLines, one, run  # noqa: E402

NOT_ON_THE_BOARD = {"crank_tooth_lost"}  # a fault of the in-memory edge model only


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def field(pattern, text, cast=str, default=None):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else default


def budget_row(path, quantity):
    """The measured value of one row of a budget file (open-tick's or open-crank's jitter), or None."""
    if not os.path.exists(path):
        return None
    with open(path) as f:
        for line in f:
            p = line.strip().split(",")
            if p[0] == quantity and len(p) >= 3 and p[2] != "":
                return float(p[2])
    return None


def load_bridge(bitstream):
    """Loads the bridge fresh. The BL616 sometimes resets its USB (it is gone for about a second), so a
    failed load is tried again, up to three times."""
    for attempt in range(3):
        print("== load the bridge into the Tang's SRAM (fresh)", flush=True)
        r = subprocess.run(["openFPGALoader", "-b", "tangnano20k", bitstream], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True)
        if r.returncode == 0:
            return
        print(f"   openFPGALoader failed (exit {r.returncode}); the BL616 may be re-enumerating", flush=True)
        time.sleep(3.0)
    print(r.stdout)
    sys.exit("hilrun: cannot load the bridge")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hil", required=True, help="open-crank-hil")
    ap.add_argument("--bitstream", required=True, help="open-tick's bridge.fs, loaded into the Tang's SRAM")
    ap.add_argument("--bridge-fidelity", default=os.path.join(ROOT, "external/open-tick/gateware/tang_nano_20k/fidelity.csv"))
    ap.add_argument("--scenarios", help="comma-separated names in scenarios/l2 (default every one the board can play)")
    ap.add_argument("--iface", default="can0")
    ap.add_argument("--no-flash", action="store_true", help="the NUCLEO already runs oc-stm32g4-ecu")
    ap.add_argument("--out", default="hilrun-out")
    a = ap.parse_args()
    if not os.access(a.hil, os.X_OK) or not os.path.isfile(a.bitstream) or not os.path.isfile(a.bridge_fidelity):
        print("hilrun: --hil must be an executable, --bitstream and --bridge-fidelity files", file=sys.stderr)
        return 2
    if not os.path.exists(f"/sys/class/net/{a.iface}"):
        print(f"hilrun: no {a.iface}; bring it up first", file=sys.stderr)
        return 2
    l2 = os.path.join(ROOT, "scenarios", "l2")
    if a.scenarios:
        names = [x.strip() for x in a.scenarios.split(",") if x.strip()]
    else:
        names = sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(l2, "*.csv"))
                       if not p.endswith(".accept.csv") and os.path.basename(p)[:-4] not in NOT_ON_THE_BOARD)
    for n in names:
        if not os.path.isfile(os.path.join(l2, n + ".csv")):
            print(f"hilrun: no scenario {n} in scenarios/l2", file=sys.stderr)
            return 2
    os.makedirs(a.out, exist_ok=True)
    vcp = one("/dev/serial/by-id/*STLINK*-if02", "ST-LINK virtual COM port")
    tang = one("/dev/serial/by-id/usb-SIPEED_USB_Debugger_*-if01-port0", "Tang Nano 20K UART")
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], stdout=subprocess.PIPE, text=True).stdout.strip()
    bmd5 = md5(a.bitstream)
    usb = os.path.realpath(f"/sys/class/tty/{os.path.basename(os.path.realpath(tang))}/device")
    with open(os.path.join(a.out, "meta.txt"), "w") as f:
        f.write(f"commit={commit}\nbitstream_md5={bmd5}\nbridge_fidelity_md5={md5(a.bridge_fidelity)}\n"
                f"tang_usb={usb}\nhost={os.uname().nodename}\niface={a.iface}\nscenarios={','.join(names)}\n")

    if not a.no_flash:
        run([os.path.join(HERE, "flash.sh"), "ecu"], "build and program oc-stm32g4-ecu")
    reader = SerialLines(vcp)
    reader.start()
    results = []
    for name in names:
        for attempt in (1, 2):
            tag = name if attempt == 1 else name + ".attempt2"
            load_bridge(a.bitstream)
            run([os.path.join(HERE, "reset.sh")], "reset the NUCLEO: the ECU starts from rest")
            time.sleep(1.0)
            print(f"== {name} (attempt {attempt})", flush=True)
            o = os.path.join(a.out, tag)
            cmd = [a.hil, os.path.join(l2, name + ".csv"), "--dev", tang, "--can", a.iface, "--quiet",
                   "--fidelity", os.path.join(ROOT, "sim/l2/fidelity.csv"), "--bridge-fidelity", a.bridge_fidelity,
                   "--trace", o + ".trace.csv", "--frames", o + ".frames.csv", "--verdicts", o + ".verdicts.csv",
                   "--jitter", o + ".jitter.csv", "--budget", o + ".budget.csv",
                   "--bitstream-md5", bmd5, "--commit", commit]
            t0 = time.monotonic()
            r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            t1 = time.monotonic()
            time.sleep(0.3)
            with open(o + ".txt", "w") as f:
                f.write(r.stdout)
            with open(o + ".usart2.log", "w") as f:
                for t, x in reader.lines:
                    if t0 <= t <= t1 + 0.3:
                        f.write(f"{t - t0:.3f} {x}\n")
            print(r.stdout, end="", flush=True)
            if r.returncode != 3:
                break
            print("   the hard run was not ok (exit 3); " + ("retrying once" if attempt == 1 else "giving up"), flush=True)
        crit = re.findall(r"^(PASS|FAIL)  ", r.stdout, re.M)
        results.append({
            "name": name, "rc": r.returncode, "attempt": attempt,
            "hard": field(r"hard run (\w+):", r.stdout, str, "?"),
            "pass": crit.count("PASS"), "n": len(crit),
            "inj": field(r", (\d+) injections,", r.stdout, int, 0),
            "misplaced": field(r"(\d+) misplaced", r.stdout, int, -1),
            "first_rev": field(r"first injection after ([\d.]+) rev", r.stdout, float, -1.0),
            "lag": field(r"capture lag max (\d+) steps", r.stdout, int, -1),
            "p99": budget_row(o + ".jitter.csv", "soi_err_p99_deg"),
            "max": budget_row(o + ".jitter.csv", "soi_err_max_deg"),
            "rtt": budget_row(o + ".budget.csv", "rtt_p99_ns"),
        })
    reader.stop = True
    reader.join()

    lines = ["scenario | criteria | hard run | injections | misplaced | first injection | capture lag max | "
             "start of injection p99 / max | round trip p99"]
    for x in results:
        p99 = f"{x['p99']:.4f}" if x["p99"] is not None else "-"
        mx = f"{x['max']:.4f}" if x["max"] is not None else "-"
        rtt = f"{x['rtt'] / 1e6:.2f} ms" if x["rtt"] is not None else "-"
        retry = " (retried)" if x["attempt"] == 2 else ""
        lines.append(f"{x['name']} | {x['pass']} of {x['n']} | {x['hard']}{retry} | {x['inj']} | {x['misplaced']} | "
                     f"{x['first_rev']:.2f} rev | {x['lag']} steps | {p99} / {mx} deg | {rtt}")
    failed = [x["name"] for x in results if x["rc"] != 0]
    lines.append("hilrun: " + ("every scenario passed" if not failed else f"FAILED: {', '.join(failed)}"))
    lines.append("the NUCLEO runs oc-stm32g4-ecu")
    with open(os.path.join(a.out, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print()
    print("\n".join(lines))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

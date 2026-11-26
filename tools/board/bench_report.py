#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bench report for tools/board/bench.sh.

Reads what bench.sh left in a run directory (meta.txt, rtt.txt, one
<scenario>.txt per acceptance scenario, and one <scenario>.mc_step.txt with
the worst observed mc_step) and the reference bench, writes
report.md in the same directory, prints it, and exits:

  0  the bench matches the reference: round trip within its limits, every
     scenario PASS with 0 late steps, every reaction within one step of the
     reference
  1  it does not (the report says why)
  2  usage, or a file missing or not understood

Python 3 standard library only.
"""
import argparse
import csv
import os
import re
import sys

STEP_MS = 10            # the controller's step (portable/machine_ctrl)
RTT_P99_MAX_MS = 5.0    # half a step: a round trip above this can move reactions by more than one step
RTT_LOST_MAX = 0
CPU_HZ = 170_000_000    # the demo's system clock (platforms/stm32g4/README.md)

SUMMARY = re.compile(r"^open-crank-accept: (PASS|FAIL|INVALID)\b.*?(\d+) late steps?")
SUMMARY_COUNTS = re.compile(r"\((\d+) of (\d+) criteria(?: failed)?\)")
CRITERION = re.compile(r"^(PASS|FAIL)\s+(\S+)\s+(.*?)(?:\s+reaction: (-?[0-9.]+) ms after .*)?$")
RTT = re.compile(r"min ([0-9.]+)\s+p50 ([0-9.]+)\s+p99 ([0-9.]+)\s+max ([0-9.]+)")


def read_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                meta[k] = v
    return meta


def read_reference(path):
    ref = {}
    with open(path) as f:
        rows = csv.DictReader(line for line in f if not line.startswith("#"))
        for r in rows:
            ref.setdefault(r["scenario"], {})[r["criterion"]] = float(r["reaction_ms"])
    return ref


def read_rtt(path):
    vals = {}
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[0] in ("sent", "received", "lost", "ignored"):
                vals[parts[0]] = int(parts[1])
            m = RTT.search(line)
            if m:
                vals["min"], vals["p50"], vals["p99"], vals["max"] = (float(x) for x in m.groups())
    return vals


def read_scenario(path):
    res = {"verdict": None, "late": None, "criteria": [], "note": None}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("bench: "):
                res["note"] = line[len("bench: "):]
            m = CRITERION.match(line)
            if m:
                res["criteria"].append((m.group(1), m.group(2),
                                        float(m.group(4)) if m.group(4) else None))
            m = SUMMARY.match(line)
            if m:
                res["verdict"], res["late"] = m.group(1), int(m.group(2))
            elif line.startswith("open-crank-accept: INVALID"):
                res["verdict"] = "INVALID"
    return res


def read_mc_step(path):
    """Cycles from <scenario>.mc_step.txt, or None if missing or unavailable."""
    try:
        with open(path) as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                if k == "mc_step_cycles_max" and v.isdigit():
                    return int(v)
    except OSError:
        pass
    return None


def fmt_us(cycles):
    return "%.1f" % (cycles * 1e6 / CPU_HZ)


def fmt_ms(x):
    return ("%d" % x) if float(x).is_integer() else ("%.3f" % x)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("run_dir")
    ap.add_argument("--reference", required=True)
    ap.add_argument("--scenarios", required=True, help="comma-separated, in run order")
    args = ap.parse_args(argv)

    try:
        meta = read_meta(os.path.join(args.run_dir, "meta.txt"))
        ref = read_reference(args.reference)
    except (OSError, KeyError, ValueError) as e:
        print("bench_report.py: %s" % e, file=sys.stderr)
        return 2

    problems = []
    out = []
    out.append("# open-crank bench report")
    out.append("")
    out.append("| | |")
    out.append("|---|---|")
    for key, label in (("date", "Date"), ("commit", "open-crank commit"), ("firmware", "Firmware (ELF SHA-256)"),
                       ("toolchain", "Toolchain"), ("openocd", "OpenOCD"), ("stlink", "ST-LINK"),
                       ("adapter", "USB-CAN adapter"), ("iface", "CAN interface")):
        out.append("| %s | %s |" % (label, meta.get(key, "?")))
    out.append("")

    # 1. Round trip
    out.append("## 1. Bus round trip (echo firmware)")
    out.append("")
    rtt_path = os.path.join(args.run_dir, "rtt.txt")
    if os.path.exists(rtt_path):
        rtt = read_rtt(rtt_path)
        if "p99" not in rtt:
            out.append("No echo received: check the wiring, the termination and the bit rate (250 kbit/s).")
            problems.append("round trip: no echo received")
        else:
            ok_lost = rtt.get("lost", 0) <= RTT_LOST_MAX
            ok_p99 = rtt["p99"] <= RTT_P99_MAX_MS
            out.append("| Sent | Lost | min | p50 | p99 | max | Limits | Result |")
            out.append("|---|---|---|---|---|---|---|---|")
            out.append("| %d | %d | %s ms | %s ms | %s ms | %s ms | lost %d, p99 ≤ %.1f ms | %s |" % (
                rtt.get("sent", 0), rtt.get("lost", 0), fmt_ms(rtt["min"]), fmt_ms(rtt["p50"]),
                fmt_ms(rtt["p99"]), fmt_ms(rtt["max"]), RTT_LOST_MAX, RTT_P99_MAX_MS,
                "OK" if ok_lost and ok_p99 else "**OUT**"))
            if not ok_lost:
                problems.append("round trip: %d frames lost" % rtt["lost"])
            if not ok_p99:
                problems.append("round trip: p99 %s ms above %.1f ms" % (fmt_ms(rtt["p99"]), RTT_P99_MAX_MS))
            out.append("")
            out.append("Reference bench: p50 1.499 ms, p99 1.735 ms, 0 lost (2026-11-24).")
    else:
        out.append("Skipped (`--no-rtt`).")
    out.append("")

    # 2. Acceptance
    out.append("## 2. Acceptance suite on the board (reference controller)")
    out.append("")
    out.append("Each scenario after a software reset of the board. Reactions in ms, with the reference bench's "
               "in brackets; a reaction matches when it is within one %d ms step of the reference." % STEP_MS)
    out.append("")
    out.append("| Scenario | Result | Late steps | Reactions (reference) | mc_step max (cycles, µs) |")
    out.append("|---|---|---|---|---|")
    worst = None  # (cycles, scenario)
    for name in [s for s in args.scenarios.split(",") if s]:
        path = os.path.join(args.run_dir, name + ".txt")
        if not os.path.exists(path):
            out.append("| `%s` | not run | | | |" % name)
            problems.append("%s: not run" % name)
            continue
        r = read_scenario(path)
        cycles = read_mc_step(os.path.join(args.run_dir, name + ".mc_step.txt"))
        if cycles is not None and (worst is None or cycles > worst[0]):
            worst = (cycles, name)
        mc_step = "%d, %s" % (cycles, fmt_us(cycles)) if cycles is not None else "not read"
        if r["verdict"] is None:
            note = r["note"] or "no verdict in the report"
            out.append("| `%s` | **%s** | | | %s |" % (name, note, mc_step))
            problems.append("%s: %s" % (name, note))
            continue
        passed = sum(1 for c in r["criteria"] if c[0] == "PASS")
        reactions = []
        for verdict, cid, ms in r["criteria"]:
            if ms is None:
                continue
            want = ref.get(name, {}).get(cid)
            if want is None:
                reactions.append("%s %s (no reference)" % (cid, fmt_ms(ms)))
            elif abs(ms - want) <= STEP_MS:
                reactions.append("%s %s (%s)" % (cid, fmt_ms(ms), fmt_ms(want)))
            else:
                reactions.append("**%s %s (%s)**" % (cid, fmt_ms(ms), fmt_ms(want)))
                problems.append("%s: %s reaction %s ms, reference %s ms" % (name, cid, fmt_ms(ms), fmt_ms(want)))
        result = "%s %d/%d" % (r["verdict"], passed, len(r["criteria"]))
        if r["verdict"] != "PASS":
            result = "**" + result + "**"
            problems.append("%s: %s" % (name, r["verdict"]))
        if r["late"]:
            problems.append("%s: %d late steps" % (name, r["late"]))
        out.append("| `%s` | %s | %s | %s | %s |" % (name, result, r["late"], ", ".join(reactions), mc_step))
    out.append("")
    if worst is None:
        out.append("worst observed mc_step: not read")
    else:
        out.append("worst observed mc_step: %d cycles (%s µs, %.2f %% of the %d ms period), in `%s`" % (
            worst[0], fmt_us(worst[0]), 100.0 * worst[0] / (CPU_HZ * STEP_MS / 1000.0), STEP_MS, worst[1]))
    out.append("")
    out.append("mc_step max: the demo's `mc_step_cycles_max` (DWT cycle counter at %d MHz), read with OpenOCD "
               "after the scenario without halting the core. It is the worst observed time, not a proven bound, "
               "and it is not a criterion of BENCH OK." % (CPU_HZ // 1_000_000))
    out.append("")

    out.append("## Verdict")
    out.append("")
    if problems:
        out.append("**BENCH DOES NOT MATCH THE REFERENCE**")
        out.append("")
        for p in problems:
            out.append("- " + p)
    else:
        out.append("**BENCH OK**: the round trip is within its limits, every scenario passes with 0 late steps, "
                   "and every reaction is within one step of the reference bench.")
    out.append("")

    text = "\n".join(out)
    with open(os.path.join(args.run_dir, "report.md"), "w") as f:
        f.write(text)
    print(text)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

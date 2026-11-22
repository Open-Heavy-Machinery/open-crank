#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare board runs of hoist_step against the paced SIL reference.

Usage:
  tools/compare_runs.py REF BOARD [BOARD ...]

REF and each BOARD are open-crank traces (CSV with a header row). The metrics
and their limits are defined in docs/board-vs-sil.md
(https://github.com/Open-Heavy-Machinery/open-crank/blob/main/docs/board-vs-sil.md),
which is the definition this script follows.

Only the columns t_s, rpm, controlling_sa (integer, 39 = 0x27) and
pump_cmd_pct are read, by name; the mc_* columns are never used. An empty
field means "no value": the row is skipped for that column.

Conventions, as in docs/board-vs-sil.md: the band is 1500 +/- 10 rpm (inside
means |rpm - 1500| <= 10); windows are half-open, [a, b); "first row at or
after" means t_s >= a; the settling time in a window is the time of the row
after the last row of the window that is outside the band, or the start of
the window if no row is outside; for a minimum or maximum that repeats, the
first row is used.

Comparison: times pass if the difference, rounded to whole milliseconds, is
at most 30 ms; droop_min_rpm and over_max_rpm within +/-5 rpm; means within
+/-1 rpm; maxdev of each BOARD at most 2 rpm; control_held must hold in REF
and in every BOARD. Differences in rpm are rounded to 6 decimals before they
are compared, so that float noise does not decide a limit.

Exit status: 0 if every metric of every BOARD passes, 1 if any fails,
2 for wrong arguments or a trace that cannot be read or measured.
"""

import argparse
import csv
import math
import sys

COLUMNS = ("t_s", "rpm", "controlling_sa", "pump_cmd_pct")
CONTROLLER_SA = 0x27
BAND_CENTRE = 1500.0
BAND_HALF = 10.0
TIME_LIMIT_MS = 30
AMPLITUDE_LIMIT_RPM = 5.0
MEAN_LIMIT_RPM = 1.0
MAXDEV_LIMIT_RPM = 2.0
STEADY_WINDOWS = ((5.0, 8.0, "5_8"), (12.0, 18.0, "12_18"), (21.0, 25.0, "21_25"))


class TraceError(Exception):
    pass


def number(text, convert, path, line, column):
    if text is None or text.strip() == "":
        return None
    try:
        return convert(text)
    except ValueError:
        raise TraceError("%s:%d: %s is not a number: %r"
                         % (path, line, column, text)) from None


def read_trace(path):
    try:
        with open(path, newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            missing = [c for c in COLUMNS if c not in (reader.fieldnames or [])]
            if missing:
                raise TraceError("%s: missing column(s): %s"
                                 % (path, ", ".join(missing)))
            rows = []
            for row in reader:
                line = reader.line_num
                t = number(row["t_s"], float, path, line, "t_s")
                if t is None:
                    continue
                rows.append({
                    "t": t,
                    "rpm": number(row["rpm"], float, path, line, "rpm"),
                    "sa": number(row["controlling_sa"], int, path, line,
                                 "controlling_sa"),
                    "pump": number(row["pump_cmd_pct"], float, path, line,
                                   "pump_cmd_pct"),
                })
    except OSError as e:
        raise TraceError("%s: %s" % (path, e.strerror or e)) from None
    if not rows:
        raise TraceError("%s: no rows" % path)
    return rows


def outside_band(rpm):
    return abs(rpm - BAND_CENTRE) > BAND_HALF


def window(rows, key, a, b, path, name):
    """Indexes of the rows in [a, b) that have a value in column key."""
    idx = [i for i, r in enumerate(rows) if a <= r["t"] < b and r[key] is not None]
    if not idx:
        raise TraceError("%s: %s: no rows with %s in [%g, %g) s"
                         % (path, name, key, a, b))
    return idx


def first_at_or_after(rows, a, key, test, path, name):
    for r in rows:
        if r["t"] >= a and r[key] is not None and test(r[key]):
            return r["t"]
    where = "" if a == -math.inf else " at or after %g s" % a
    raise TraceError("%s: %s: no row%s matches" % (path, name, where))


def settling(rows, a, b, path, name):
    idx = window(rows, "rpm", a, b, path, name)
    outside = [i for i in idx if outside_band(rows[i]["rpm"])]
    if not outside:
        return a
    after = outside[-1] + 1
    if after >= len(rows):
        raise TraceError("%s: %s: no row after the last row outside the band"
                         % (path, name))
    return rows[after]["t"]


def extreme(rows, a, b, pick, path, name):
    """(time, rpm) of the first row with the min (pick=min) or max rpm."""
    idx = window(rows, "rpm", a, b, path, name)
    best = idx[0]
    for i in idx[1:]:
        if pick(rows[i]["rpm"], rows[best]["rpm"]) != rows[best]["rpm"]:
            best = i
    return rows[best]["t"], rows[best]["rpm"]


def metrics(rows, path):
    m = {}
    takeover = first_at_or_after(rows, -math.inf, "sa",
                                 lambda sa: sa == CONTROLLER_SA, path,
                                 "takeover_s")
    m["takeover_s"] = takeover
    m["reach_band_s"] = first_at_or_after(rows, 2.0, "rpm",
                                          lambda v: not outside_band(v), path,
                                          "reach_band_s")
    m["settle_start_s"] = settling(rows, 2.0, 8.0, path, "settle_start_s")
    m["pump_on_s"] = first_at_or_after(rows, 8.0, "pump", lambda v: v > 1.0,
                                       path, "pump_on_s")
    droop_t, droop_rpm = extreme(rows, 8.0, 12.0,
                                 lambda x, y: x if x < y else y, path,
                                 "droop_min_rpm")
    m["droop_t_s"] = droop_t
    m["settle_load_s"] = settling(rows, 8.0, 18.0, path, "settle_load_s")
    m["pump_off_s"] = first_at_or_after(rows, 18.0, "pump", lambda v: v < 1.0,
                                        path, "pump_off_s")
    over_t, over_rpm = extreme(rows, 18.0, 21.0,
                               lambda x, y: x if x > y else y, path,
                               "over_max_rpm")
    m["over_t_s"] = over_t
    m["settle_unload_s"] = settling(rows, 18.0, 25.0, path, "settle_unload_s")
    m["droop_min_rpm"] = droop_rpm
    m["over_max_rpm"] = over_rpm
    for a, b, tag in STEADY_WINDOWS:
        idx = window(rows, "rpm", a, b, path, "mean_" + tag)
        values = [rows[i]["rpm"] for i in idx]
        m["mean_" + tag] = sum(values) / len(values)
        m["maxdev_" + tag] = max(abs(v - BAND_CENTRE) for v in values)
    m["control_held"] = all(r["sa"] == CONTROLLER_SA
                            for r in rows if r["t"] >= takeover)
    return m


TIME_METRICS = ("takeover_s", "reach_band_s", "settle_start_s", "pump_on_s",
                "droop_t_s", "settle_load_s", "pump_off_s", "over_t_s",
                "settle_unload_s")
AMPLITUDE_METRICS = ("droop_min_rpm", "over_max_rpm")
MEAN_METRICS = tuple("mean_" + t for _, _, t in STEADY_WINDOWS)
MAXDEV_METRICS = tuple("maxdev_" + t for _, _, t in STEADY_WINDOWS)


def compare(ref, board):
    """Rows (metric, ref, board, diff, limit, passed) for one board run."""
    out = []
    for name in TIME_METRICS:
        diff_ms = round((board[name] - ref[name]) * 1000.0)
        out.append((name, "%.3f" % ref[name], "%.3f" % board[name],
                    "%+d ms" % diff_ms, "±%d ms" % TIME_LIMIT_MS,
                    abs(diff_ms) <= TIME_LIMIT_MS))
    for names, limit in ((AMPLITUDE_METRICS, AMPLITUDE_LIMIT_RPM),
                         (MEAN_METRICS, MEAN_LIMIT_RPM)):
        for name in names:
            diff = round(board[name] - ref[name], 6)
            out.append((name, "%.2f" % ref[name], "%.2f" % board[name],
                        "%+.2f rpm" % diff, "±%g rpm" % limit,
                        abs(diff) <= limit))
    for name in MAXDEV_METRICS:
        out.append((name, "-", "%.2f" % board[name], "-",
                    "≤ %g rpm" % MAXDEV_LIMIT_RPM,
                    round(board[name], 6) <= MAXDEV_LIMIT_RPM))
    yes = {True: "yes", False: "no"}
    out.append(("control_held", yes[ref["control_held"]],
                yes[board["control_held"]], "-", "must hold",
                ref["control_held"] and board["control_held"]))
    return out


def print_table(label, rows):
    header = ("metric", "ref", "board", "diff", "limit", "result")
    cells = [header] + [r[:5] + ("PASS" if r[5] else "FAIL",) for r in rows]
    widths = [max(len(c[i]) for c in cells) for i in range(len(header))]
    print(label)
    for c in cells:
        print("  " + "  ".join(v.ljust(w) for v, w in zip(c, widths)).rstrip())
    print()


def main(argv):
    parser = argparse.ArgumentParser(
        description="Compare board runs of hoist_step against the paced SIL "
                    "reference (docs/board-vs-sil.md).")
    parser.add_argument("ref", metavar="REF", help="reference trace")
    parser.add_argument("boards", metavar="BOARD", nargs="+",
                        help="board-in-the-loop trace")
    args = parser.parse_args(argv)

    try:
        ref = metrics(read_trace(args.ref), args.ref)
        boards = [(p, metrics(read_trace(p), p)) for p in args.boards]
    except TraceError as e:
        print("compare_runs: %s" % e, file=sys.stderr)
        return 2

    summary = []
    for path, board in boards:
        rows = compare(ref, board)
        print_table(path, rows)
        failed = sum(1 for r in rows if not r[5])
        summary.append((path, failed))
    for path, failed in summary:
        print("%s: %s" % (path, "PASS" if failed == 0
                          else "FAIL (%d metrics)" % failed))
    return 0 if all(f == 0 for _, f in summary) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

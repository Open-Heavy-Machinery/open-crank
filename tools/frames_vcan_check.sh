#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The frame log over SocketCAN (open-crank-accept --frames): with the
# reference controller on the bus, one short scenario must pass, the log must
# say bus_clock=kernel, and every frame must carry a kernel bus time (so every
# frame the bench sent was matched with its own copy).
#
# Usage: tools/frames_vcan_check.sh <build-dir> <can-interface>
set -euo pipefail

BUILD=${1:?usage: tools/frames_vcan_check.sh <build-dir> <can-interface>}
IFACE=${2:?usage: tools/frames_vcan_check.sh <build-dir> <can-interface>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$BUILD/frames"
mkdir -p "$OUT"

"$BUILD/open-crank-mc" "$IFACE" > "$OUT/mc.log" 2>&1 &
MC=$!
trap 'kill $MC 2>/dev/null || true' EXIT
sleep 0.5
rc=0
"$BUILD/open-crank-accept" "$IFACE" "$ROOT/scenarios/cold_start.csv" --frames "$OUT/cold_start.frames.csv" \
    > "$OUT/cold_start.txt" 2>&1 || rc=$?
tail -1 "$OUT/cold_start.txt"
if [[ $rc -eq 3 ]]; then
    echo "frames_vcan_check: INVALID run (late steps on this runner); the frame log is not judged"
    exit 0
fi
[[ $rc -eq 0 ]] || { echo "frames_vcan_check: the acceptance run failed (exit $rc)"; exit 1; }

python3 - "$OUT/cold_start.frames.csv" <<'PY'
import csv, sys
path = sys.argv[1]
lines = open(path).read().splitlines()
meta = lines[0]
if not meta.startswith("# otk-frame-log 1;") or "bus_clock=kernel" not in meta:
    sys.exit(f"frames_vcan_check: unexpected metadata line: {meta}")
rows = list(csv.DictReader(lines[1:]))
dirs = {d: sum(1 for r in rows if r["dir"] == d) for d in ("to_dut", "from_dut")}
no_bus = [r for r in rows if not r["t_bus_s"]]
print(f"frames_vcan_check: {len(rows)} frames ({dirs['to_dut']} to the DUT, {dirs['from_dut']} from it), "
      f"{len(no_bus)} without a bus time")
if dirs["to_dut"] == 0 or dirs["from_dut"] == 0 or no_bus:
    sys.exit("frames_vcan_check: FAIL")
print("frames_vcan_check: OK")
PY

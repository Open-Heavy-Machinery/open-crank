#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The README's vcan demo, automated: the reference controller and the
# simulated machine as two processes on a virtual CAN bus, paced by the wall
# clock. Checks what a person watching candump would check:
#
#   - the controller reaches RUN and never faults;
#   - before the hoist load (7.0-7.9 s) the engine holds 1500 rpm within 1 %
#     and reports the controller (0x27) in control of its speed;
#   - under the load (11.0-12.0 s) it is back within 1 % of 1500 rpm.
#
# Needs a CAN interface; set one up as the README says.
#
# Usage: tools/vcan_demo_check.sh <build dir> [interface]
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <build dir> [interface]" >&2
    exit 2
fi
BUILD=$1
IF=${2:-vcan0}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; [[ -n "${MC:-}" ]] && kill -INT "$MC" 2>/dev/null || true' EXIT

"$BUILD/open-crank-mc" "$IF" >"$WORK/mc.log" &
MC=$!
sleep 0.5
timeout -s INT 13 "$BUILD/open-crank-sil" "$IF" "$ROOT/scenarios/hoist_step.csv" --trace "$WORK/sil.csv" \
    >"$WORK/sil.log" || [[ $? -eq 124 ]]
kill -INT "$MC"
wait "$MC" || true
MC=

status=0
check() {
    if eval "$2"; then
        echo "ok    $1"
    else
        echo "FAIL  $1"
        status=1
    fi
}

check "controller reached RUN" "grep -q -- '-> RUN' '$WORK/mc.log'"
check "controller never faulted" "! grep -q -- '-> FAULT' '$WORK/mc.log'"

# Trace columns: 1 t_s, 6 rpm, 17 controlling_sa.
window() { # from to max_dev_rpm need_sa
    awk -F, -v a="$1" -v b="$2" -v d="$3" -v sa="$4" '
        NR > 1 && $1 >= a && $1 <= b {
            n++
            dev = $6 - 1500; if (dev < 0) dev = -dev
            if (dev > d) bad++
            if (sa != "" && $17 != sa) bad++
        }
        END { exit (n > 0 && bad == 0) ? 0 : 1 }' "$WORK/sil.csv"
}
check "1500 rpm within 1 % and controller in control, 7.0-7.9 s" "window 7.0 7.9 15 39"
check "back within 1 % of 1500 rpm under the hoist load, 11.0-12.0 s" "window 11.0 12.0 15 ''"

if [[ $status -ne 0 ]]; then
    echo "--- controller log"
    tail -20 "$WORK/mc.log"
    echo "--- machine log"
    tail -20 "$WORK/sil.log"
fi
exit $status

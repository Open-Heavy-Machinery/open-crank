#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The watchdog test on the board (docs/safety.md, SR-11; v0.3.1 step 3c).
#
#   1. builds the host runner and the firmware from this commit
#   2. programs oc-stm32g4-demo-hangtest: the reference controller plus an
#      intentional hang on the bench's test frame (OC_WDG_HANG_TEST). That
#      build exists only for this test; CI checks that the reference
#      firmware has no hang in it
#   3. resets the board, waits for its TSC1 and plays
#      scenarios/board/watchdog_reset.csv with open-crank-accept: at 8 s the
#      bench sends the test frame, the loop stops, the IWDG (100 ms) restarts
#      the board, and the controller must start in FAULT until the fault reset
#   4. always programs the reference controller (oc-stm32g4-demo) back,
#      whatever happened, so the board is never left with the test build
#
# Usage: tools/board/watchdog_check.sh [--iface can0] [--out DIR]
# Exit 0 PASS, 1 FAIL, 2 usage or setup.
set -euo pipefail
source "$(dirname "$0")/common.sh"

iface=can0
out=""
while [[ $# -gt 0 ]]; do
    case $1 in
        --iface) iface=$2; shift 2 ;;
        --out)   out=$2; shift 2 ;;
        -h|--help) sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "watchdog_check.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac
done
out=${out:-$OC_ROOT/watchdog-$(date +%Y%m%d-%H%M%S)}
scenario=$OC_ROOT/scenarios/board/watchdog_reset.csv

ip link show "$iface" 2>/dev/null | grep -q 'state UP\|,UP' || {
    echo "watchdog_check.sh: CAN interface $iface is not up (see tools/board/bench.sh)" >&2; exit 2; }

echo "== build (host runner and firmware, this commit)"
oc_use_toolchain
cmake -S "$OC_ROOT" -B "$OC_ROOT/build" >/dev/null
cmake --build "$OC_ROOT/build" --target open-crank-accept >/dev/null
cmake -S "$OC_ROOT/platforms/stm32g4" -B "$OC_FW_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$OC_ROOT/cmake/arm-none-eabi.cmake" -DOC_WERROR=ON >/dev/null
cmake --build "$OC_FW_BUILD" >/dev/null
mkdir -p "$out"

restore() {
    echo "== program the reference controller back"
    "$OC_ROOT/tools/board/flash.sh" demo --no-build >"$out/flash-demo.log" 2>&1 ||
        echo "watchdog_check.sh: WARNING: programming the reference controller failed; see $out/flash-demo.log" >&2
}
trap restore EXIT

echo "== program the hang-test build (test only)"
"$OC_ROOT/tools/board/flash.sh" demo-hangtest --no-build >"$out/flash-hangtest.log" 2>&1 || {
    echo "watchdog_check.sh: programming the hang-test build failed; see $out/flash-hangtest.log" >&2; exit 2; }

"$OC_ROOT/tools/board/reset.sh" >/dev/null
if ! timeout 2 candump -n 1 "$iface,0C000027:1FFFFFFF" >/dev/null 2>&1; then
    echo "watchdog_check.sh: no TSC1 from the board within 2 s of the reset" >&2; exit 2
fi

echo "== watchdog_reset (18 s)"
code=0
"$OC_ROOT/build/open-crank-accept" "$iface" "$scenario" --trace "$out/watchdog_reset.csv" \
    --verdicts "$out/watchdog_reset.verdicts.csv" | tee "$out/watchdog_reset.txt" || code=$?
case $code in
    0) echo "watchdog_check.sh: PASS; results in $out" ;;
    1) echo "watchdog_check.sh: FAIL; results in $out" ;;
    *) echo "watchdog_check.sh: no verdict (exit $code); results in $out" ;;
esac
exit $code

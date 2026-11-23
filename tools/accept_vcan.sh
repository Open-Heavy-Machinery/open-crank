#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The acceptance suite on a virtual CAN bus: for every scenarios/*.accept.csv,
# a fresh reference controller (open-crank-mc: a new process is a reset) and
# open-crank-accept with the matching scenario, paced by the wall clock. The
# runner grades the controller only by the frames it sends; see
# docs/acceptance.md.
#
# Each run's output is printed and kept in <build dir>/accept/<scenario>.log,
# the controller's in <scenario>.mc.log.
#
# Needs a CAN interface; set one up as the README says.
#
# Usage: tools/accept_vcan.sh <build dir> [interface]
# Exit:  0 every scenario passes; 1 some scenario fails; 3 some run is
#        INVALID (late steps) and none fails.
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <build dir> [interface]" >&2
    exit 2
fi
BUILD=$1
IF=${2:-vcan0}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$BUILD/accept"
mkdir -p "$OUT"
trap '[[ -n "${MC:-}" ]] && kill -INT "$MC" 2>/dev/null || true' EXIT

shopt -s nullglob
criteria=("$ROOT"/scenarios/*.accept.csv)
if [[ ${#criteria[@]} -eq 0 ]]; then
    echo "no scenarios/*.accept.csv found" >&2
    exit 2
fi

summary=()
failed=0
invalid=0
for crit in "${criteria[@]}"; do
    name=$(basename "$crit" .accept.csv)
    scenario="$ROOT/scenarios/$name.csv"
    echo "=== $name"

    "$BUILD/open-crank-mc" "$IF" >"$OUT/$name.mc.log" &
    MC=$!
    sleep 0.5
    code=0
    timeout -s INT 120 "$BUILD/open-crank-accept" "$IF" "$scenario" --criteria "$crit" \
        >"$OUT/$name.log" 2>&1 || code=$?
    kill -INT "$MC" 2>/dev/null || true
    wait "$MC" || true
    MC=
    cat "$OUT/$name.log"

    case $code in
    0) verdict=PASS ;;
    3) verdict=INVALID; invalid=1 ;;
    *) verdict=FAIL; failed=1 ;;
    esac
    summary+=("$(printf '%-8s %s' "$verdict" "$name")")
done

echo
echo "=== acceptance suite on $IF"
printf '%s\n' "${summary[@]}"
if [[ $failed -ne 0 ]]; then
    exit 1
fi
if [[ $invalid -ne 0 ]]; then
    exit 3
fi
exit 0

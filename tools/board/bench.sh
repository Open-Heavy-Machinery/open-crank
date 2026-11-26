#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The standard bench check: is this physical setup right, and does the
# reference controller run on it as on the reference bench? One command, no
# CubeIDE, no button. It is also the easiest way to watch the demo run.
#
#   1. setup.sh --check: tools, toolchain and permissions are in place
#   2. builds the host tools and both firmwares from this commit
#   3. programs the echo firmware and measures the bus round trip
#      (tools/can_rtt.py): wiring, termination, adapter and bit rate
#   4. programs the reference controller (program + verify: the board runs
#      exactly this build), then for every acceptance scenario: resets the
#      board, waits until it sends TSC1 (alive; a fresh reset starts the
#      controller in WAIT_ENGINE, not in FAULT), runs open-crank-accept,
#      then reads the worst observed mc_step (mc_step_cycles_max, DWT cycles)
#      from the running board with OpenOCD, without halting the core
#   5. writes report.md: the setup, the commit, the round trip, every verdict
#      and every reaction time against the reference bench
#      (tools/board/bench_reference.csv), and the worst observed mc_step per
#      scenario (a measurement, not a criterion of BENCH OK)
#
# Usage: tools/board/bench.sh [--iface can0] [--out DIR] [--rtt-count N] [--no-rtt] [--include-serials] [SCENARIO...]
#   --iface       SocketCAN interface of the USB-CAN adapter (default can0)
#   --out         run directory (default bench-<date>-<time>/ at the top of the repository)
#   --rtt-count   round-trip requests (default 1000, at 100 per second)
#   --no-rtt      skip the round trip
#   --include-serials
#                 record the serial numbers of the ST-LINK and the USB-CAN adapter
#                 in meta.txt and report.md. By default they are written as <serial>,
#                 so a report can be shared as it is.
#   SCENARIO      names under scenarios/ (default: every scenario with an .accept.csv)
#
# Exit 0 bench OK, 1 the bench does not match the reference, 2 usage or setup.
set -euo pipefail
source "$(dirname "$0")/common.sh"

iface=can0
out=""
rtt_count=1000
rtt=1
serials=0
scenarios=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --iface)     iface=$2; shift 2 ;;
        --out)       out=$2; shift 2 ;;
        --rtt-count) rtt_count=$2; shift 2 ;;
        --no-rtt)    rtt=0; shift ;;
        --include-serials) serials=1; shift ;;
        -h|--help)   sed -n '4,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)          echo "bench.sh: unknown option '$1'" >&2; exit 2 ;;
        *)           scenarios+=("$1"); shift ;;
    esac
done
if [[ ${#scenarios[@]} -eq 0 ]]; then
    for f in "$OC_ROOT"/scenarios/*.accept.csv; do
        scenarios+=("$(basename "$f" .accept.csv)")
    done
fi
for s in "${scenarios[@]}"; do
    [[ -f $OC_ROOT/scenarios/$s.csv ]] || { echo "bench.sh: no scenario scenarios/$s.csv" >&2; exit 2; }
done
out=${out:-$OC_ROOT/bench-$(date +%Y%m%d-%H%M%S)}

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }

step "1/5 setup"
"$OC_ROOT/tools/board/setup.sh" --check >/dev/null || {
    "$OC_ROOT/tools/board/setup.sh" --check | grep -E 'MISSING' || true
    echo "bench.sh: the PC is not ready; run tools/board/setup.sh" >&2
    exit 2
}
if ! ip link show "$iface" 2>/dev/null | grep -q 'state UP\|,UP'; then
    echo "bench.sh: CAN interface $iface is not up. With a CANable-class adapter (slcan, 250 kbit/s):" >&2
    for p in /dev/serial/by-id/*; do
        [[ -e $p && $p != *STLINK* && $p != *STMicro* ]] && echo "  sudo slcand -o -c -s5 $p $iface && sudo ip link set $iface up" >&2
    done
    exit 2
fi
echo "PC ready; $iface is up"

step "2/5 build (host tools and firmware, this commit)"
oc_use_toolchain
cmake -S "$OC_ROOT" -B "$OC_ROOT/build" >/dev/null
cmake --build "$OC_ROOT/build" --target open-crank-accept >/dev/null
cmake -S "$OC_ROOT/platforms/stm32g4" -B "$OC_FW_BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$OC_ROOT/cmake/arm-none-eabi.cmake" -DOC_WERROR=ON >/dev/null
cmake --build "$OC_FW_BUILD" >/dev/null
mkdir -p "$out"
demo_elf=$OC_FW_BUILD/oc-stm32g4-demo.elf
# Where the demo keeps its worst observed mc_step, in the ELF that is programmed.
mc_step_addr=$(arm-none-eabi-nm "$demo_elf" | awk '$3 == "mc_step_cycles_max" { print $1 }')
[[ -n $mc_step_addr ]] || { echo "bench.sh: mc_step_cycles_max not found in $demo_elf" >&2; exit 2; }
commit=$(git -C "$OC_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
git -C "$OC_ROOT" diff --quiet HEAD -- 2>/dev/null || commit="$commit (with local changes)"
# A /dev/serial/by-id name ends in _<serial>-ifNN; mask the serial unless --include-serials.
mask() { if [[ $serials == 1 ]]; then cat; else sed -E 's/_[^_]+(-if[0-9]+)$/_<serial>\1/'; fi; }
by_id() { for p in /dev/serial/by-id/*"$1"*; do [[ -e $p ]] && basename "$p"; done | head -1 | mask; }
{
    echo "date=$(date '+%Y-%m-%d %H:%M:%S %z')"
    echo "commit=$commit"
    echo "firmware=oc-stm32g4-demo.elf $(sha256sum "$demo_elf" | cut -c1-16)…"
    echo "toolchain=$(arm-none-eabi-gcc --version | head -1)"
    echo "openocd=$(openocd --version 2>&1 | head -1)"
    echo "stlink=$(by_id STLINK || echo '?')"
    echo "adapter=$(for p in /dev/serial/by-id/*; do [[ -e $p && $p != *STLINK* ]] && basename "$p"; done | head -1 | mask)"
    echo "iface=$iface"
} > "$out/meta.txt"
echo "commit $commit; results in $out"

if [[ $rtt == 1 ]]; then
    step "3/5 bus round trip (echo firmware, $rtt_count requests)"
    "$OC_ROOT/tools/board/flash.sh" echo --no-build >"$out/flash-echo.log" 2>&1 || {
        echo "bench.sh: programming the echo failed; see $out/flash-echo.log" >&2; exit 1; }
    sleep 0.3
    python3 "$OC_ROOT/tools/can_rtt.py" "$iface" --count "$rtt_count" --csv "$out/rtt.csv" | tee "$out/rtt.txt" || true
else
    step "3/5 bus round trip: skipped"
fi

# The demo's mc_step_cycles_max, read while the core runs: OpenOCD's init does
# not halt it, and mdw goes through the ST-LINK's AHB access port. Halting would
# change the timing being measured. Prints the value in cycles, or "unavailable".
read_mc_step() {
    local word
    word=$("${OC_OPENOCD[@]}" -c "init" -c "mdw 0x$mc_step_addr" -c "shutdown" 2>&1 |
        awk -v a="0x$mc_step_addr:" 'tolower($1) == a { print $2 }')
    if [[ $word =~ ^[0-9a-fA-F]{8}$ ]]; then
        echo "mc_step_cycles_max=$((16#$word))"
    else
        echo "mc_step_cycles_max=unavailable"
    fi
}

step "4/5 reference controller, ${#scenarios[@]} scenarios (watch the bus: candump $iface)"
"$OC_ROOT/tools/board/flash.sh" demo --no-build >"$out/flash-demo.log" 2>&1 || {
    echo "bench.sh: programming the demo failed; see $out/flash-demo.log" >&2; exit 1; }
i=0
for s in "${scenarios[@]}"; do
    i=$((i + 1))
    # Progress: which scenario, of how many, and how long it plays (its end event).
    dur=$(awk -F, '$2 == "end" { print $1 }' "$OC_ROOT/scenarios/$s.csv")
    printf '\n-- %s (%d of %d, %s s)\n' "$s" "$i" "${#scenarios[@]}" "${dur:-?}"
    "$OC_ROOT/tools/board/reset.sh" >/dev/null || { echo "bench: reset failed" > "$out/$s.txt"; continue; }
    # Alive: the controller sends TSC1 (0x0C000027) every 10 ms from boot.
    if ! timeout 2 candump -n 1 "$iface,0C000027:1FFFFFFF" >/dev/null 2>&1; then
        echo "bench: no TSC1 from the board within 2 s of the reset" | tee "$out/$s.txt"
        continue
    fi
    "$OC_ROOT/build/open-crank-accept" "$iface" "$OC_ROOT/scenarios/$s.csv" --trace "$out/$s.csv" \
        --verdicts "$out/$s.verdicts.csv" \
        | tee "$out/$s.txt" || true
    # Before the next reset, which clears it.
    read_mc_step | tee "$out/$s.mc_step.txt"
done

step "5/5 report"
scen_list=$(IFS=,; echo "${scenarios[*]}")
set +e
python3 "$OC_ROOT/tools/board/bench_report.py" "$out" \
    --reference "$OC_ROOT/tools/board/bench_reference.csv" --scenarios "$scen_list"
status=$?
set -e
echo
echo "bench.sh: report in $out/report.md; traces in $out/*.csv (open them in open-rig)"
exit $status

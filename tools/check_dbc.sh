#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# open-crank.dbc agrees with open-crank's encoders (tools/check_dbc.py), and
# the check can fail: each mutation of the DBC below must be caught.
#
# Usage: tools/check_dbc.sh <build-dir>   (needs oc-dbc-vectors built, and
#        cantools: pip install -r tools/requirements-dbc.txt)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:?usage: tools/check_dbc.sh <build-dir>}
VECTORS="$BUILD/tests/oc-dbc-vectors"
DBC="$ROOT/open-crank.dbc"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

"$VECTORS" > "$WORK/vectors.txt"
python3 "$ROOT/tools/check_dbc.py" "$DBC" < "$WORK/vectors.txt"

problems=0
# mutate <description> <sed expression>
mutate() {
    sed "$2" "$DBC" > "$WORK/mutant.dbc"
    if cmp -s "$WORK/mutant.dbc" "$DBC"; then
        echo "STALE     $1  (pattern not found in open-crank.dbc)"
        problems=$((problems + 1))
    elif python3 "$ROOT/tools/check_dbc.py" "$WORK/mutant.dbc" < "$WORK/vectors.txt" > "$WORK/out.txt"; then
        echo "SURVIVED  $1"
        problems=$((problems + 1))
    else
        echo "killed    $1  [$(grep -m1 MISMATCH "$WORK/out.txt" | cut -c11-90)]"
    fi
}

mutate "EEC1 engine speed with the wrong scale" 's/EngineSpeed : 24|16@1+ (0.125,0)/EngineSpeed : 24|16@1+ (0.25,0)/'
mutate "TSC1 checksum overlapping the counter" 's/MessageChecksum : 60|4/MessageChecksum : 56|4/'
mutate "EFL/P1 oil pressure in the wrong byte" 's/EngineOilPressure : 24|8/EngineOilPressure : 16|8/'
mutate "ET1 coolant with the wrong offset" 's/ (1,-40) / (1,-50) /'
mutate "EFL/P1 period not the bench's" 's/BA_ "GenMsgCycleTime" BO_ 2566844160 500;/BA_ "GenMsgCycleTime" BO_ 2566844160 100;/'
mutate "the controller's DM1 period not the controller's" 's/BA_ "GenMsgCycleTime" BO_ 2566834727 1000;/BA_ "GenMsgCycleTime" BO_ 2566834727 500;/'
mutate "the controller's DM1 missing" '/^BO_ 2566834727 DM1_MC/,/^$/d'
mutate "OC_OPERATOR loses the hoist lever" '/SG_ HoistLever/d'
mutate "not marked as J1939" 's/BA_ "ProtocolType" "J1939";//'

echo
[[ $problems -eq 0 ]] && echo "DBC mutations: all killed" || { echo "DBC mutations: $problems problems"; exit 1; }

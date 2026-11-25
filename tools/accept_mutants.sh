#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Deliberately broken controllers must fail the acceptance suite (exit
# criterion 2 of v0.2.1; docs/acceptance.md). Each mutation below breaks one
# behaviour of the reference controller, in a scratch copy of the tree, as
# tools/mutation_check.sh does; the mutant is then graded on every
# scenario with open-crank-accept-loop (the in-memory loop: deterministic,
# fast, no vcan) and must fail the criteria listed with it.
#
# Every mutant runs all the scenarios, without stopping at the first failure:
# the full list says which criteria have teeth.
#
#   killed      every expected criterion failed (other failures are listed too)
#   accepted    an accepted survivor survived, as documented
#   SURVIVED    no criterion failed
#   WRONG       some criterion failed, but not every expected one
#   UNEXPECTED  an accepted survivor is now killed: update docs/acceptance.md
#   ERROR       the runner did not finish a scenario (exit code other than 0 or 1)
#   STALE       the mutation's code pattern no longer exists: update it
#   NO BUILD    the mutation does not compile
#
# Usage: tools/accept_mutants.sh   (needs cmake, a C compiler, python3)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

tar -C "$ROOT" --exclude=./build --exclude='./build-*' --exclude=./.git --exclude=./.cache -cf - . | tar -C "$WORK" -xf -
cmake -S "$WORK" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$WORK/build" -j >/dev/null

RUNNER="$WORK/build/open-crank-accept-loop"
SCENARIOS=()
for criteria in "$WORK"/scenarios/*.accept.csv; do
    SCENARIOS+=("$(basename "$criteria" .accept.csv)")
done
if [[ ${#SCENARIOS[@]} -eq 0 ]]; then
    echo "no scenarios/*.accept.csv found" >&2
    exit 2
fi

# grade: runs every scenario. Sets $failed_ids (space-separated, all
# scenarios), $failed_list ("<scenario>: <ids>; ...") and $errors (scenarios
# the runner did not finish).
grade() {
    failed_ids=""
    failed_list=""
    errors=""
    local name code ids
    for name in "${SCENARIOS[@]}"; do
        code=0
        "$RUNNER" "$WORK/scenarios/$name.csv" >"$WORK/$name.out" 2>&1 || code=$?
        if [[ $code -ne 0 && $code -ne 1 ]]; then
            errors="$errors $name (exit $code)"
            continue
        fi
        ids=$(sed -n 's/^FAIL  \([^ ]*\)  .*$/\1/p' "$WORK/$name.out" | tr '\n' ' ')
        ids=${ids% }
        if [[ -n $ids ]]; then
            failed_ids="$failed_ids $ids"
            failed_list="$failed_list${failed_list:+; }$name: ${ids// /, }"
        fi
    done
    errors=${errors# }
}

grade
if [[ -n $failed_ids || -n $errors ]]; then
    echo "The reference controller already fails the acceptance suite; fix that first."
    exit 1
fi

killed=0
accepted=0
problems=0

# mutate <description> <file> <exact text> <replacement> <criteria that must fail | accepted>
mutate() {
    local what=$1 file=$2 from=$3 to=$4 expected=$5
    local path="$WORK/$file"
    cp "$path" "$path.orig"
    if ! python3 - "$path" "$from" "$to" <<'EOF'
import sys
path, old, new = sys.argv[1:4]
text = open(path).read()
if old not in text:
    sys.exit(1)
open(path, "w").write(text.replace(old, new, 1))
EOF
    then
        echo "STALE     $what  (pattern not found in $file)"
        problems=$((problems + 1))
        mv "$path.orig" "$path"
        touch "$path" # newer than the mutated object, so the next build restores it
        return
    fi
    if ! cmake --build "$WORK/build" -j >/dev/null 2>&1; then
        echo "NO BUILD  $what  (the mutation does not compile)"
        problems=$((problems + 1))
    else
        grade
        if [[ -n $errors ]]; then
            echo "ERROR     $what  the runner did not finish: $errors"
            problems=$((problems + 1))
        elif [[ $expected == accepted ]]; then
            if [[ -z $failed_ids ]]; then
                echo "accepted  $what  (survives; accepted survivor, see docs/acceptance.md)"
                accepted=$((accepted + 1))
            else
                echo "UNEXPECTED  $what  the accepted survivor is now killed [$failed_list]; update docs/acceptance.md"
                problems=$((problems + 1))
            fi
        elif [[ -z $failed_ids ]]; then
            echo "SURVIVED  $what"
            problems=$((problems + 1))
        else
            local id missing=""
            for id in $expected; do
                if [[ " $failed_ids " != *" $id "* ]]; then
                    missing="$missing $id"
                fi
            done
            if [[ -z $missing ]]; then
                echo "killed    $what  [$failed_list]"
                killed=$((killed + 1))
            else
                echo "WRONG     $what  expected $expected, failed [$failed_list]"
                problems=$((problems + 1))
            fi
        fi
    fi
    mv "$path.orig" "$path"
    touch "$path" # newer than the mutated object, so the next build restores it
}

MC=portable/machine_ctrl/src/machine_ctrl.c

mutate "controller: engine status timeout ignored" $MC \
    "if (!status_fresh) {" "if (false) {" \
    "E2"
mutate "controller: overspeed ignored" $MC \
    "} else if (c->engine_rpm > p->overspeed_rpm) {" "} else if (false) {" \
    "V1"
mutate "controller: never checks that the engine honours the override" $MC \
    "} else if (override_rejected(c, now_ms)) {" "} else if (false) {" \
    "O1 L1"
mutate "controller: pump stays commanded outside RUN" $MC \
    "c->pump_cmd_pct = run ? hoist_pct : 0.0f;" "c->pump_cmd_pct = hoist_pct;" \
    "G2 E2"
mutate "controller: override kept outside RUN" $MC \
    ".override_mode = run ? J1939_OVERRIDE_SPEED : J1939_OVERRIDE_DISABLED," ".override_mode = J1939_OVERRIDE_SPEED," \
    "S1 E2 O1 V1 L1"
mutate "controller: operator target not clamped to the governed range" $MC \
    "? clampf(c->op.target_speed_rpm, p->request_min_rpm, p->request_max_rpm)" "? c->op.target_speed_rpm" \
    "G1"
mutate "controller: speed request not slew-limited" $MC \
    "c->requested_rpm += clampf(c->target_rpm - c->requested_rpm, -max_delta, max_delta);" \
    "c->requested_rpm = c->target_rpm; (void)max_delta;" \
    accepted

# One per link-fault scenario (open-crank v0.3).
mutate "controller: EEC1 timeout multiplied by 5" $MC \
    "!elapsed(now_ms, c->eec1_ms, p->eec1_timeout_ms + 1U)" "!elapsed(now_ms, c->eec1_ms, (5U * p->eec1_timeout_ms) + 1U)" \
    "D1"
mutate "controller: faults on the first late EEC1 (EEC1 timeout cut to 20 ms)" $MC \
    "!elapsed(now_ms, c->eec1_ms, p->eec1_timeout_ms + 1U)" "!elapsed(now_ms, c->eec1_ms, (p->eec1_timeout_ms / 10U) + 1U)" \
    "J1"
mutate "controller: sends only every 150 ms (no margin for a lost frame)" $MC \
    "size_t n = 0U;" "size_t n = 0U; if ((now_ms % 150U) != 0U) { return 0U; }" \
    "I2"
mutate "controller: waits 5 s for the engine to honour the override" $MC \
    ".override_confirm_ms = 500U," ".override_confirm_ms = 5000U," \
    "B1"

# TSC1 counter and checksum (open-crank v0.3.1, step 3b).
mutate "controller: TSC1 checksum not written" portable/j1939/src/j1939.c \
    "f->data[7] = (uint8_t)((uint8_t)(j1939_tsc1_checksum(f, counter) << 4U) | counter);" "f->data[7] = counter;" \
    "G3"
mutate "controller: TSC1 counter frozen" $MC \
    "c->tsc1_counter = (uint8_t)((uint8_t)(c->tsc1_counter + 1U) & 0x0FU);" "(void)0;" \
    "G3"

echo
echo "$killed mutants killed, $accepted accepted survivors, $problems problems"
[[ $problems -eq 0 ]]

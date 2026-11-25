#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Mutation sanity check (ARCHITECTURE.md, section 6): a test that cannot fail
# proves nothing. Each mutation below breaks one safety-relevant behaviour on
# purpose, in a scratch copy of the tree; the test suite must then fail.
#
# A mutation "survives" if the suite still passes: that behaviour is not
# really tested. A mutation is "stale" if its code pattern no longer exists:
# update the pattern when you change the code.
#
# Usage: tools/mutation_check.sh   (needs cmake, a C compiler, python3)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

tar -C "$ROOT" --exclude=./build --exclude='./build-*' --exclude=./.git --exclude=./.cache -cf - . | tar -C "$WORK" -xf -
cmake -S "$WORK" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$WORK/build" -j >/dev/null
if ! ctest --test-dir "$WORK/build" >/dev/null 2>&1; then
    echo "The unmutated tree already fails its tests; fix that first."
    exit 1
fi

killed=0
problems=0

# mutate <description> <file> <exact text> <replacement>
mutate() {
    local what=$1 file=$2 from=$3 to=$4
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
    elif ctest --test-dir "$WORK/build" >/dev/null 2>&1; then
        echo "SURVIVED  $what"
        problems=$((problems + 1))
    else
        failing=$( (ctest --test-dir "$WORK/build" 2>/dev/null || true) | sed -n 's/^.*[0-9] - \([a-z0-9_]*\) (Failed).*$/\1/p' | tr '\n' ' ')
        echo "killed    $what  [${failing% }]"
        killed=$((killed + 1))
    fi
    mv "$path.orig" "$path"
    touch "$path" # newer than the mutated object, so the next build restores it
}

ECU=sim/engine_ecu/src/engine_ecu.c
MC=portable/machine_ctrl/src/machine_ctrl.c
PLANT=sim/core/src/plant.c

mutate "plant: combustion torque on a stationary shaft (self-start)" $PLANT \
    "if (rpm > 0.0) {" "if (rpm >= 0.0) {"
mutate "plant: friction subtracted twice from the full-load table" $PLANT \
    "plant_full_load_brake_nm(p, rpm) + p->friction_c0_nm + p->friction_c1_nm_per_rpm * rpm;" \
    "plant_full_load_brake_nm(p, rpm);"
mutate "plant: no air-path lag" $PLANT \
    "const double burnt_frac = min2(fuel_pct / 100.0, air_frac);" "const double burnt_frac = fuel_pct / 100.0; (void)air_frac;"
mutate "engine ECU: stale speed requests never time out" $ECU \
    "age_ms <= p->request_timeout_ms" "(age_ms <= p->request_timeout_ms || true)"
mutate "engine ECU: overspeed never trips" $ECU \
    "e->overspeed_tripped = true;" "e->overspeed_tripped = false;"
mutate "engine ECU: overspeed trip not latched until standstill" $ECU \
    "} else if (e->overspeed_tripped && rpm < ECU_STOPPED_RPM) {" "} else if (e->overspeed_tripped) {"
mutate "engine ECU: fuel below synchronisation speed" $ECU \
    "fuel = (rpm >= p->fuel_enable_rpm) ? p->start_fuel_pct : 0.0;" "fuel = p->start_fuel_pct;"
mutate "engine ECU: stall not detected" $ECU \
    "if (rpm < p->stall_rpm) {" "if (false) {"
mutate "engine ECU: speed requests not clamped to the governed range" $ECU \
    "clamp(e->request.speed_rpm, p->low_idle_rpm, p->high_idle_rpm)" "e->request.speed_rpm"
mutate "controller: engine status timeout ignored" $MC \
    "if (!status_fresh) {" "if (false) {"
mutate "controller: overspeed ignored" $MC \
    "} else if (c->engine_rpm > p->overspeed_rpm) {" "} else if (false) {"
mutate "controller: never checks that the engine honours the override" $MC \
    "} else if (override_rejected(c, now_ms)) {" "} else if (false) {"
mutate "controller: pump stays commanded outside RUN" $MC \
    "c->pump_cmd_pct = run ? hoist_pct : 0.0f;" "c->pump_cmd_pct = hoist_pct;"
mutate "controller: override kept outside RUN" $MC \
    ".override_mode = run ? J1939_OVERRIDE_SPEED : J1939_OVERRIDE_DISABLED," ".override_mode = J1939_OVERRIDE_SPEED,"
mutate "controller: speed request not slew-limited" $MC \
    "c->requested_rpm += clampf(c->target_rpm - c->requested_rpm, -max_delta, max_delta);" \
    "c->requested_rpm = c->target_rpm; (void)max_delta;"
mutate "controller: operator target not clamped to the governed range" $MC \
    "? clampf(c->op.target_speed_rpm, p->request_min_rpm, p->request_max_rpm)" "? c->op.target_speed_rpm"
mutate "codec: 16-bit values do not saturate" portable/j1939/src/j1939_scaling.h \
    "const float raw = j1939_clampf((value - offset) / resolution, 0.0f, (float)J1939_U16_MAX_VALID);" \
    "const float raw = (value - offset) / resolution;"
mutate "codec: 'not available' decoded as a value" portable/j1939/src/j1939_scaling.h \
    "if (raw <= J1939_U16_MAX_VALID) {" "if (true) {"
# The parser now lives in open-tick, whose own mutation check covers it. This
# one breaks open-crank's binding to it instead.
mutate "scenario: events never reach the bench's signals" sim/runtime/src/bench.c \
    "oc_signals_set((OcSignals *)ctx, (OcSignalId)signal, value);" "(void)ctx; (void)signal; (void)value;"
mutate "controller: after a watchdog restart it starts in WAIT_ENGINE" $MC \
    "    c->state = MC_STATE_FAULT;
    c->fault = MC_FAULT_WATCHDOG_RESET;" "    c->fault = MC_FAULT_NONE;"
mutate "controller: a button held through a watchdog restart counts as a press" $MC \
    "c->reset_was_pressed = true; /* a button held through the restart is not a new press */" "(void)0;"
mutate "codec: DM1 drops the SPN's high bits" portable/j1939/src/j1939_diag.c \
    "((uint32_t)((p[2] >> 5U) & 0x07U) << 16U)" "0U"
mutate "codec: BAM accepts a packet out of sequence" portable/j1939/src/j1939_diag.c \
    "if (late || (seq != rx->next_seq)) {" "if (late) {"
mutate "codec: BAM ignores the T1 timeout" portable/j1939/src/j1939_diag.c \
    "if (late || (seq != rx->next_seq)) {" "if (seq != rx->next_seq) {"
mutate "controller: limp-home pump limit steps instead of ramping" $MC \
    "c->pump_limit_pct += clampf(limit_target - c->pump_limit_pct, -limit_step, limit_step);" "c->pump_limit_pct = limit_target; (void)limit_step;"
mutate "controller: limp-home does not cap the requested speed" $MC \
    "c->target_rpm = p->limp_rpm; /* limp-home; the slew limit makes the change smooth */" "(void)0;"
mutate "controller: the red stop lamp does not block a new RUN" $MC \
    "if (status_fresh && (c->engine_rpm >= p->running_rpm) && !c->engine_red_stop) {" "if (status_fresh && (c->engine_rpm >= p->running_rpm)) {"
mutate "codec: TSC1 checksum not written" portable/j1939/src/j1939.c \
    "f->data[7] = (uint8_t)((uint8_t)(j1939_tsc1_checksum(f, counter) << 4U) | counter);" "f->data[7] = counter;"
mutate "codec: TSC1 checksum leaves out the identifier" portable/j1939/src/j1939.c \
    "sum += (f->id >> 24U) & 0xFFU;" "sum = (uint32_t)counter & 0x0FU; for (uint32_t k = 0U; k < 7U; k++) { sum += (uint32_t)f->data[k]; }"
mutate "bench: the engine ECU accepts a TSC1 with a wrong checksum" sim/runtime/src/bench.c \
    "if (!tsc1.checksum_valid || repeated) {" "if (repeated) {"
mutate "bench: the engine ECU accepts a repeated TSC1 counter" sim/runtime/src/bench.c \
    "if (!tsc1.checksum_valid || repeated) {" "if (!tsc1.checksum_valid) {"
mutate "oracle: tsc1_intact ignores the counter" sim/accept/src/accept.c \
    "(tsc1.checksum_valid && next) ? 1.0 : 0.0" "(tsc1.checksum_valid || !next) ? 1.0 : 0.0"

echo
echo "$killed mutations killed, $problems problems"
[[ $problems -eq 0 ]]

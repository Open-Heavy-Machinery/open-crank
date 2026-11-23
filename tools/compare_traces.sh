#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Behaviour check: runs every scenario with two builds of open-crank-run and
# compares the traces byte for byte. A refactor (such as moving code into
# open-tick) must leave every trace identical; a pull request that changes
# behaviour on purpose says so with the `behaviour-change` label, which turns
# differences into warnings.
#
# A scenario the base build cannot run (a new file, a new signal) is reported
# as not comparable, not as a failure.
#
# Usage: tools/compare_traces.sh <base build dir> <head build dir> <scenario dir>
#   BEHAVIOUR_CHANGE=true   differences are expected: warn instead of failing
#   GITHUB_STEP_SUMMARY     if set, a table of results is appended to it
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <base build dir> <head build dir> <scenario dir>" >&2
    exit 2
fi
BASE=$1/open-crank-run
HEAD=$2/open-crank-run
SCENARIOS=$3
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

summary() {
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        echo "$1" >>"$GITHUB_STEP_SUMMARY"
    fi
}

summary "| Scenario | Trace |"
summary "|---|---|"
different=0
compared=0
for scenario in "$SCENARIOS"/*.csv; do
    [[ $scenario == *.accept.csv ]] && continue # acceptance criteria, not a scenario
    name=$(basename "$scenario" .csv)
    "$HEAD" "$scenario" --quiet --trace "$WORK/$name.head.csv" >/dev/null
    if ! "$BASE" "$scenario" --quiet --trace "$WORK/$name.base.csv" >/dev/null 2>&1; then
        echo "not comparable  $name  (the base build cannot run it)"
        summary "| \`$name\` | not comparable: the base build cannot run it |"
        continue
    fi
    compared=$((compared + 1))
    if cmp -s "$WORK/$name.base.csv" "$WORK/$name.head.csv"; then
        echo "identical       $name"
        summary "| \`$name\` | identical |"
    else
        first=$( (cmp "$WORK/$name.base.csv" "$WORK/$name.head.csv" || true) | sed -E 's/.*line ([0-9]+).*/\1/')
        echo "DIFFERENT       $name  (first difference on line $first)"
        diff <(sed -n "${first}p" "$WORK/$name.base.csv") <(sed -n "${first}p" "$WORK/$name.head.csv") || true
        summary "| \`$name\` | **different** from line $first |"
        different=$((different + 1))
    fi
done

echo
echo "$compared compared, $different different"
if [[ $different -gt 0 ]]; then
    if [[ "${BEHAVIOUR_CHANGE:-false}" == "true" ]]; then
        echo "::warning::$different trace(s) changed; the pull request is labelled behaviour-change"
        exit 0
    fi
    echo "Traces changed. If that is intended, label the pull request 'behaviour-change' and explain why."
    exit 1
fi

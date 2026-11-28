#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The Level 2 wiring diagrams: platforms/stm32g4/wiring-l2.yml and
# wiring-l2-analog.yml, drawn by WireViz into a .gv (Graphviz source) and an
# .svg that GitHub shows in WIRING-L2.md.
#
# Usage:
#   tools/wireviz.sh            regenerate the .gv and .svg next to each .yml
#   tools/wireviz.sh --check    regenerate into a temporary directory and fail if
#                               a committed .gv differs, byte for byte; check that
#                               each committed .svg is the render of its .gv, and
#                               every signal against WIRING-L2.md and open-tick's
#                               tang_nano_20k.cst (tools/check_wiring.py)
#
# WireViz and its Python dependencies are installed without sudo into a venv of
# their own, every file pinned by SHA-256 (tools/wireviz-requirements.lock, pip
# --require-hashes, wheels only), in $OC_WIREVIZ_DIR or
# ${XDG_CACHE_HOME:-~/.cache}/open-crank/wireviz. It needs CPython 3.10 to 3.14.
#
# Graphviz (`dot`) comes from the system, not pinned: install it with your
# package manager (Debian/Ubuntu: apt install graphviz). Its .svg output
# depends on the machine's fonts and Graphviz build, so the check compares the
# .gv byte for byte (WireViz's own output, pinned), and the .svg by content.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOCK=$ROOT/tools/wireviz-requirements.lock
DIR=${OC_WIREVIZ_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/open-crank/wireviz}
VENV=$DIR/venv
STEMS=(wiring-l2 wiring-l2-analog)
BOARD=$ROOT/platforms/stm32g4

check=0
case ${1:-} in
    --check) check=1 ;;
    "") ;;
    -h|--help) sed -n '4,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "wireviz.sh: unknown argument '$1'" >&2; exit 2 ;;
esac

# Every Python call runs isolated from the user's environment: no PYTHONPATH
# (ROS sets one), no user site-packages, no pip configuration files.
py_clean() { env -u PYTHONPATH -u PYTHONHOME PYTHONNOUSERSITE=1 PIP_CONFIG_FILE=/dev/null "$@"; }

command -v dot >/dev/null || {
    echo "wireviz.sh: Graphviz's dot is needed (Debian/Ubuntu: sudo apt install graphviz)" >&2
    exit 2
}

lock_sha=$(sha256sum "$LOCK" | cut -d' ' -f1)
if [[ ! -f $VENV/.lock.sha256 || $(cat "$VENV/.lock.sha256") != "$lock_sha" ]]; then
    command -v python3 >/dev/null || { echo "wireviz.sh: python3 is needed" >&2; exit 2; }
    pyv=$(py_clean python3 -I -c 'import sys; print("%d.%d" % sys.version_info[:2])')
    case $pyv in
        3.1[0-4]) ;;
        *) echo "wireviz.sh: python3 is $pyv; the lock needs CPython 3.10 to 3.14 (put one first on PATH)" >&2; exit 2 ;;
    esac
    echo "wireviz.sh: creating $VENV from wireviz-requirements.lock" >&2
    rm -rf "$VENV"
    mkdir -p "$DIR"
    py_clean python3 -I -m venv "$VENV"
    py_clean "$VENV/bin/python" -I -m pip install --isolated --disable-pip-version-check --no-input -q \
        --require-hashes --only-binary :all: -r "$LOCK" >&2
    echo "$lock_sha" > "$VENV/.lock.sha256"
fi
echo "wireviz.sh: WireViz $(py_clean "$VENV/bin/wireviz" -V 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)," \
     "$(dot -V 2>&1 | head -1)" >&2

if [[ $check -eq 0 ]]; then
    for s in "${STEMS[@]}"; do
        py_clean "$VENV/bin/wireviz" -f gs "$BOARD/$s.yml" >/dev/null
        echo "wireviz.sh: platforms/stm32g4/$s.gv, $s.svg" >&2
    done
    exit 0
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
status=0
for s in "${STEMS[@]}"; do
    py_clean "$VENV/bin/wireviz" -f g -o "$WORK" "$BOARD/$s.yml" >/dev/null
    if cmp -s "$WORK/$s.gv" "$BOARD/$s.gv"; then
        echo "ok    $s.gv is what WireViz makes of $s.yml"
    else
        echo "FAIL  $s.gv is not what WireViz makes of $s.yml: run tools/wireviz.sh and commit the result"
        diff "$BOARD/$s.gv" "$WORK/$s.gv" | head -20 || true
        status=1
    fi
done
py_clean "$VENV/bin/python" -I "$ROOT/tools/check_wiring.py" --svg || status=1
exit $status

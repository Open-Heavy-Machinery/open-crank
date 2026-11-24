#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Reset the NUCLEO-G431KB through its ST-LINK and let it run: the same as
# pressing the reset button, from a script. Run it before each scenario (the
# controller latches FAULT when open-crank-sil stops; see README.md).
#
# Usage: tools/board/reset.sh
# Exit 0 reset, 1 OpenOCD failed, 2 OpenOCD missing (run tools/board/setup.sh).
set -euo pipefail
source "$(dirname "$0")/common.sh"

oc_need openocd
"${OC_OPENOCD[@]}" -c "init; reset run; shutdown" 2>/dev/null || {
    echo "reset.sh: OpenOCD could not reset the board (is the ST-LINK connected? try: ${OC_OPENOCD[*]} -c 'init; shutdown')" >&2
    exit 1
}
echo "reset.sh: board reset and running"

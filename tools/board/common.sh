# SPDX-License-Identifier: Apache-2.0
# Shared by the board scripts (sourced, not run).

OC_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
OC_FW_BUILD=${OC_FW_BUILD:-$OC_ROOT/build-stm32g4}

# OpenOCD for the NUCLEO-G431KB's on-board ST-LINK. The reset is a system reset
# requested over SWD: no button, no NRST wire.
OC_OPENOCD=(openocd -f interface/stlink.cfg -f target/stm32g4x.cfg)

oc_need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "$(basename "$0"): '$1' not found; run tools/board/setup.sh" >&2
        exit 2
    }
}

# Puts the pinned Arm toolchain first on PATH.
oc_use_toolchain() {
    local bin
    bin=$("$OC_ROOT/tools/board/arm_toolchain.sh" --check) || exit 2
    PATH="$bin:$PATH"
    export PATH
}

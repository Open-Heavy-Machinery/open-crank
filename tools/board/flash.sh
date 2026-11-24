#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build the board firmware with the pinned toolchain and program it into the
# NUCLEO-G431KB through its ST-LINK: program, verify, reset. No CubeIDE and no
# button.
#
# Usage: tools/board/flash.sh [demo|echo] [--no-build]
#   demo        the reference controller (default)
#   echo        the round-trip echo for tools/can_rtt.py
#   --no-build  program what is already in build-stm32g4/
#
# Exit 0 programmed and verified, 1 build or programming failed, 2 usage or a
# missing tool (run tools/board/setup.sh).
set -euo pipefail
source "$(dirname "$0")/common.sh"

app=demo
build=1
for arg in "$@"; do
    case $arg in
        demo|echo)  app=$arg ;;
        --no-build) build=0 ;;
        -h|--help)  sed -n '4,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)          echo "flash.sh: unknown argument '$arg'" >&2; exit 2 ;;
    esac
done

oc_need openocd
elf=$OC_FW_BUILD/oc-stm32g4-$app.elf

if [[ $build == 1 ]]; then
    oc_need cmake
    oc_use_toolchain
    echo "== build ($(arm-none-eabi-gcc -dumpversion))"
    cmake -S "$OC_ROOT/platforms/stm32g4" -B "$OC_FW_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$OC_ROOT/cmake/arm-none-eabi.cmake" -DOC_WERROR=ON >/dev/null || exit 1
    cmake --build "$OC_FW_BUILD" --target "oc-stm32g4-$app" || exit 1
fi
[[ -f $elf ]] || { echo "flash.sh: $elf not found; build it first" >&2; exit 2; }

echo "== program $elf"
"${OC_OPENOCD[@]}" -c "program {$elf} verify reset exit" || exit 1
echo "flash.sh: $app programmed, verified and running"

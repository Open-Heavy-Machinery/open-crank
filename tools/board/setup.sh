#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Get a Linux PC ready to build, program and test the reference board: checks
# what is there, says what is missing, and installs it after asking.
#
#   - apt packages (sudo, after a y/N question): cmake, a host C compiler,
#     git, curl, python3, openocd, can-utils
#   - the pinned Arm GNU Toolchain (no sudo, into a user cache):
#     tools/board/arm_toolchain.sh
#   - permissions: the user in the plugdev (ST-LINK) and dialout (USB-CAN
#     adapter) groups, and OpenOCD's udev rules; only reported, never changed
#   - hardware, if plugged in: the Nucleo's ST-LINK and a CANable-class
#     adapter; only reported
#
# Usage: tools/board/setup.sh [--yes] [--check]
#   --yes    do not ask before installing
#   --check  install nothing; exit 1 if anything needed is missing
#
# Debian and Ubuntu (apt). On other distributions it checks and tells you
# what to install.
set -euo pipefail
source "$(dirname "$0")/common.sh"

ask=1
install=1
for arg in "$@"; do
    case $arg in
        --yes)     ask=0 ;;
        --check)   install=0 ;;
        -h|--help) sed -n '4,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)         echo "setup.sh: unknown argument '$arg'" >&2; exit 2 ;;
    esac
done

ok()   { printf '  ok       %s\n' "$*"; }
miss() { printf '  MISSING  %s\n' "$*"; }
note() { printf '  note     %s\n' "$*"; }

# command -> apt package
declare -A PKG=(
    [cmake]=cmake [cc]=build-essential [git]=git [curl]=curl [python3]=python3
    [openocd]=openocd [candump]=can-utils [slcand]=can-utils
)
missing_pkgs=()

echo "== tools"
for cmd in cmake cc git curl python3 openocd candump slcand; do
    if command -v "$cmd" >/dev/null 2>&1; then
        ok "$cmd ($(command -v "$cmd"))"
    else
        miss "$cmd (apt package ${PKG[$cmd]})"
        missing_pkgs+=("${PKG[$cmd]}")
    fi
done
if command -v cmake >/dev/null 2>&1; then
    v=$(cmake --version | head -1 | awk '{print $3}')
    if [[ $(printf '%s\n3.20\n' "$v" | sort -V | head -1) != 3.20 ]]; then
        miss "cmake $v is older than 3.20"
    fi
fi

toolchain_ok=1
if bin=$("$OC_ROOT/tools/board/arm_toolchain.sh" --check 2>/dev/null); then
    ok "Arm GNU Toolchain ($("$bin/arm-none-eabi-gcc" -dumpversion), $bin)"
else
    miss "Arm GNU Toolchain 14.2.rel1 (tools/board/arm_toolchain.sh, no sudo)"
    toolchain_ok=0
fi

if [[ $install == 1 ]] && { [[ ${#missing_pkgs[@]} -gt 0 ]] || [[ $toolchain_ok == 0 ]]; }; then
    mapfile -t missing_pkgs < <(printf '%s\n' "${missing_pkgs[@]}" | sort -u | sed '/^$/d')
    echo "== install"
    [[ ${#missing_pkgs[@]} -gt 0 ]] && echo "  sudo apt-get install -y ${missing_pkgs[*]}"
    [[ $toolchain_ok == 0 ]] && echo "  tools/board/arm_toolchain.sh   (about 150 MB, into ${OC_TOOLCHAIN_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/open-crank})"
    reply=y
    if [[ $ask == 1 ]]; then
        read -r -p "Proceed? [y/N] " reply
    fi
    if [[ $reply == [yY]* ]]; then
        if [[ ${#missing_pkgs[@]} -gt 0 ]]; then
            if command -v apt-get >/dev/null 2>&1; then
                sudo apt-get update
                sudo apt-get install -y --no-install-recommends "${missing_pkgs[@]}"
            else
                echo "setup.sh: no apt-get here; install the packages above with your package manager" >&2
            fi
        fi
        if [[ $toolchain_ok == 0 ]]; then
            "$OC_ROOT/tools/board/arm_toolchain.sh" >/dev/null && toolchain_ok=1
        fi
        exec "$0" --check   # check again, from the top
    fi
fi

echo "== permissions"
perm_ok=1
for g in plugdev dialout; do
    if id -nG | tr ' ' '\n' | grep -qx "$g"; then
        ok "user in group $g"
    else
        miss "user not in group $g: sudo usermod -aG $g \$USER, then log out and in"
        perm_ok=0
    fi
done
if compgen -G "/usr/lib/udev/rules.d/*openocd*" >/dev/null || compgen -G "/lib/udev/rules.d/*openocd*" >/dev/null \
    || compgen -G "/etc/udev/rules.d/*openocd*" >/dev/null; then
    ok "OpenOCD udev rules (ST-LINK usable without sudo)"
else
    miss "OpenOCD udev rules: they come with the openocd package"
    perm_ok=0
fi

echo "== hardware (only reported)"
if command -v lsusb >/dev/null 2>&1; then
    # ST-LINK/V2-1, V3 and V3E product IDs.
    if lsusb | grep -qiE '0483:(374b|374e|374f|3752|3753|3754)'; then
        ok "ST-LINK: $(lsusb | grep -iE '0483:37' | head -1 | cut -d' ' -f6-)"
    else
        note "no ST-LINK on USB (plug in the Nucleo to program it)"
    fi
    if lsusb | grep -qiE '16d0:117e|1d50:606f|ad50:60c4'; then
        ok "USB-CAN adapter: $(lsusb | grep -iE '16d0:117e|1d50:606f|ad50:60c4' | head -1 | cut -d' ' -f6-)"
    else
        note "no CANable-class adapter on USB"
    fi
else
    note "lsusb not found (package usbutils): hardware not checked"
fi
for p in /dev/serial/by-id/*; do
    [[ -e $p ]] && note "serial device: $p"
done

echo
if [[ ${#missing_pkgs[@]} -gt 0 || $toolchain_ok == 0 || $perm_ok == 0 ]]; then
    echo "setup.sh: something is missing (see MISSING above)"
    exit 1
fi
echo "setup.sh: ready. Next: tools/board/flash.sh demo"

#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The pinned Arm GNU Toolchain (arm-none-eabi) for the board firmware and the
# Cortex-M4F checks: the same version locally and in CI, so the stack and size
# figures in the docs can be reproduced. Installed without sudo into a user
# cache; nothing outside it is touched.
#
# Usage:
#   tools/board/arm_toolchain.sh            install if missing, print the bin directory
#   tools/board/arm_toolchain.sh --check    print the bin directory, exit 1 if not installed
#
# The directory is $OC_TOOLCHAIN_DIR, or ${XDG_CACHE_HOME:-~/.cache}/open-crank.
# Messages go to stderr; stdout is only the bin directory, so a script can do
#   PATH="$(tools/board/arm_toolchain.sh):$PATH"
set -euo pipefail

VERSION=14.2.rel1
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)  HOST=x86_64;  SHA256=62a63b981fe391a9cbad7ef51b17e49aeaa3e7b0d029b36ca1e9c3b2a9b78823 ;;
    Linux-aarch64) HOST=aarch64; SHA256=87330bab085dd8749d4ed0ad633674b9dc48b237b61069e3b481abd364d0a684 ;;
    *)
        echo "arm_toolchain.sh: no pinned build for $(uname -s) $(uname -m); install arm-none-eabi-gcc $VERSION yourself and put it on PATH" >&2
        exit 2
        ;;
esac

NAME=arm-gnu-toolchain-$VERSION-$HOST-arm-none-eabi
URL=https://developer.arm.com/-/media/Files/downloads/gnu/$VERSION/binrel/$NAME.tar.xz
DIR=${OC_TOOLCHAIN_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/open-crank}
BIN=$DIR/$NAME/bin

if [[ -x $BIN/arm-none-eabi-gcc ]]; then
    echo "$BIN"
    exit 0
fi
if [[ ${1:-} == --check ]]; then
    echo "arm_toolchain.sh: Arm GNU Toolchain $VERSION is not installed in $DIR (run tools/board/setup.sh)" >&2
    exit 1
fi

mkdir -p "$DIR"
tmp=$(mktemp -d "$DIR/.download.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
echo "arm_toolchain.sh: downloading $NAME (about 150 MB) into $DIR" >&2
curl -fsSL --retry 3 -o "$tmp/$NAME.tar.xz" "$URL"
echo "$SHA256  $tmp/$NAME.tar.xz" | sha256sum -c --quiet - >&2 || {
    echo "arm_toolchain.sh: SHA-256 mismatch for $NAME.tar.xz; not installed" >&2
    exit 1
}
tar -xJf "$tmp/$NAME.tar.xz" -C "$tmp"
mv "$tmp/$NAME" "$DIR/$NAME"
echo "arm_toolchain.sh: installed $("$BIN/arm-none-eabi-gcc" --version | head -1)" >&2
echo "$BIN"

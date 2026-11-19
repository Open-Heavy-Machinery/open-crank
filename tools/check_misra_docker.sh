#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Runs tools/check_misra.sh inside ubuntu:26.04, with the cppcheck and python3
# that apt installs there: the same environment as the CI misra job
# (runs-on: ubuntu-26.04). The baseline depends on the cppcheck version, so
# use this when your local cppcheck is a different one.
#
# Usage: tools/check_misra_docker.sh            compare with the baseline
#        tools/check_misra_docker.sh --update   regenerate the baseline
# Needs Docker. Files it writes belong to the calling user.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=ubuntu:26.04

case "${1:-}" in
    "" | --update) ;;
    *) echo "usage: tools/check_misra_docker.sh [--update]" >&2; exit 2 ;;
esac

if ! command -v docker >/dev/null 2>&1; then
    echo "FAIL: Docker is not installed. Install it, or run tools/check_misra.sh with a local cppcheck." >&2
    exit 1
fi

echo "== $IMAGE: installing cppcheck and python3"
exec docker run --rm -v "$ROOT":/src -w /src \
    -e DEBIAN_FRONTEND=noninteractive -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$IMAGE" bash -c '
        set -euo pipefail
        apt-get update -qq >/dev/null
        apt-get install -y -qq --no-install-recommends cppcheck python3 >/dev/null
        status=0
        tools/check_misra.sh "$@" || status=$?
        # Files created in the container belong to root; hand them back.
        chown "$HOST_UID:$HOST_GID" build build/misra-report.txt tools/misra-baseline.txt 2>/dev/null || true
        exit $status
    ' bash "$@"

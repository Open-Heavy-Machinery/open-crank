#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Portability check for portable/ (ARCHITECTURE.md, section 3).
#
# 1. portable/ builds on its own for Cortex-M4F (hard-float, freestanding) with
#    -Wconversion -Wdouble-promotion as errors, in Debug and MinSizeRel.
# 2. The objects, linked together, need nothing from outside except the four
#    functions GCC requires from every freestanding environment (memcpy,
#    memmove, memset, memcmp). A stray printf, malloc or soft-double helper
#    (__aeabi_d*, i.e. `double` emulated in software) fails the check.
# 3. Every #include in portable/ names one of <stdint.h>, <stdbool.h>,
#    <stddef.h>, or a header that lives inside portable/.
# 4. Stack usage: the MinSizeRel build uses -fstack-usage, and the stack usage
#    of every function is printed, largest first. Every function must be
#    `static` (a fixed amount of stack): portable code must not use a
#    variable-size stack. The call graph (-fcallgraph-info=su) gives the
#    worst-case stack of each public controller function;
#    tools/stack_budget.py fails on recursion, indirect calls or a path above
#    its limit (1024 bytes).
#
# Usage: tools/check_portable.sh   (needs arm-none-eabi-gcc and cmake)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

ALLOWED_UNDEFINED="memcpy memmove memset memcmp"
status=0

for build_type in Debug MinSizeRel; do
    echo "== Cortex-M4F build ($build_type)"
    # CFLAGS is added to the toolchain file's flags; -DCMAKE_C_FLAGS would replace them.
    stack_flags=""
    [[ $build_type == MinSizeRel ]] && stack_flags="-fstack-usage -fcallgraph-info=su"
    CFLAGS="$stack_flags" cmake -S "$ROOT/portable" -B "$WORK/$build_type" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" \
        -DCMAKE_BUILD_TYPE="$build_type" -DOC_WERROR=ON >/dev/null
    cmake --build "$WORK/$build_type" >/dev/null

    lib="$WORK/$build_type/libopencrank_portable.a"
    mkdir -p "$WORK/$build_type/objs"
    (cd "$WORK/$build_type/objs" && arm-none-eabi-ar x "$lib")
    arm-none-eabi-ld -r -o "$WORK/$build_type/portable.o" "$WORK/$build_type"/objs/*

    undefined=$(arm-none-eabi-nm -u "$WORK/$build_type/portable.o" | awk '{print $2}' | sort -u)
    for sym in $undefined; do
        if [[ " $ALLOWED_UNDEFINED " != *" $sym "* ]]; then
            echo "FAIL: portable/ needs '$sym' from outside"
            if [[ "$sym" == __aeabi_d* || "$sym" == __aeabi_*2d ]]; then
                echo "      (a double operation emulated in software: use float)"
            fi
            status=1
        fi
    done
    echo "   undefined symbols: $(echo ${undefined:-none}) (allowed: $ALLOWED_UNDEFINED)"
    arm-none-eabi-size "$WORK/$build_type/portable.o" | tail -1 | awk '{print "   size: text " $1 " B, data " $2 " B, bss " $3 " B"}'
done

echo "== Stack usage (MinSizeRel, -fstack-usage), largest first"
# Each .su line: <file>:<line>:<column>:<function><TAB><bytes><TAB><qualifier>
# Only the library's objects: CMake's compiler checks also leave .su files.
mapfile -t su_files < <(find "$WORK/MinSizeRel/CMakeFiles/opencrank_portable.dir" -name '*.su' | sort)
if [[ ${#su_files[@]} -eq 0 ]]; then
    echo "FAIL: the MinSizeRel build produced no .su files"
    status=1
else
    stack_table=$(awk -F'\t' '{ n = split($1, loc, ":"); printf "%s\t%s\t%s\n", loc[n], $2, $3 }' "${su_files[@]}" | sort -t$'\t' -k2,2nr -k1,1)
    printf 'function\tbytes\tqualifier\n%s\n' "$stack_table" | awk -F'\t' '
        { f[NR] = $1; b[NR] = $2; q[NR] = $3; if (length($1) > w) w = length($1) }
        END { for (i = 1; i <= NR; i++) printf "   %-*s  %5s  %s\n", w, f[i], b[i], q[i] }'
    while IFS=$'\t' read -r fn bytes qualifier; do
        if [[ "$qualifier" != static ]]; then
            echo "FAIL: '$fn' uses a '$qualifier' stack ($bytes bytes); portable/ must use a fixed-size stack"
            status=1
        fi
    done <<<"$stack_table"
fi

echo "== Stack budget (MinSizeRel, call graph)"
mapfile -t ci_files < <(find "$WORK/MinSizeRel/CMakeFiles/opencrank_portable.dir" -name '*.ci' | sort)
if [[ ${#ci_files[@]} -eq 0 ]]; then
    echo "FAIL: the MinSizeRel build produced no .ci files"
    status=1
elif ! "$ROOT/tools/stack_budget.py" "${ci_files[@]}" | sed 's/^/   /'; then
    status=1
fi

echo "== #include check"
while IFS= read -r -d '' file; do
    while IFS= read -r line; do
        header=$(sed -E 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]+)[>"].*/\1/' <<<"$line")
        case "$line" in
            *'<stdint.h>'* | *'<stdbool.h>'* | *'<stddef.h>'*) continue ;;
        esac
        if [[ "$line" == *'"'* ]]; then
            # A quoted include must resolve to a file inside portable/.
            resolved=$(realpath -m "$(dirname "$file")/$header")
            [[ -f "$resolved" && "$resolved" == "$ROOT/portable/"* ]] && continue
        elif [[ "$header" != *..* ]] && compgen -G "$ROOT/portable/*/include/$header" >/dev/null; then
            continue
        fi
        echo "FAIL: ${file#"$ROOT"/}: '$header' is not allowed in portable/"
        status=1
    done < <(grep -E '^[[:space:]]*#[[:space:]]*include' "$file" || true)
done < <(find "$ROOT/portable" -name '*.[ch]' -print0)

if [[ $status -eq 0 ]]; then
    echo "portable/ OK"
fi
exit $status

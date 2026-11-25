#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Worst-case stack of the controller's public functions, from GCC's call graph.

Usage:
  tools/stack_budget.py [--limit BYTES] CI_FILE [CI_FILE ...]

CI_FILE are the .ci files GCC writes with -fcallgraph-info=su (one per
object, VCG format). A node whose label ends in "N bytes (qualifier)" is a
function defined in that object, with its own stack; a node without bytes
("shape : ellipse") is a function called there but defined elsewhere. The
files are merged by function name.

Roots are the public functions of portable/machine_ctrl: mc_init,
mc_init_after_watchdog, mc_handle_frame, mc_step and mc_clear_fault. The worst-case stack of a
function is its own stack plus the largest worst-case stack among the
functions it calls. memcpy, memmove, memset and memcmp count as 0 bytes and
are listed as not counted (C library).

It fails (exit 1) on recursion (a cycle in the call graph), on an indirect
call (GCC's "__indirect_call" node), on a stack qualifier other than
"static", on a called function that no .ci defines (other than the four
above), and on a root whose worst-case stack is above --limit (default 1024
bytes). Exit 2 for wrong arguments or unreadable files.
"""

import argparse
import re
import sys

ROOTS = ("mc_init", "mc_init_after_watchdog", "mc_handle_frame", "mc_step", "mc_clear_fault")
ALLOWED_EXTERNAL = {"memcpy", "memmove", "memset", "memcmp"}
INDIRECT = "__indirect_call"

NODE = re.compile(r'^node:\s*\{\s*title:\s*"([^"]*)"\s+label:\s*"([^"]*)"')
EDGE = re.compile(r'^edge:\s*\{\s*sourcename:\s*"([^"]*)"\s+targetname:\s*"([^"]*)"')
BYTES = re.compile(r'\\n(\d+) bytes \(([^)]*)\)$')


def short(title):
    """GCC titles a file-local (static) function "<file>:<name>"; show <name>."""
    return title.rsplit(":", 1)[-1]


def read_graph(paths):
    """({function: (bytes, qualifier, file)}, {function: [callee, ...]})."""
    defined = {}
    calls = {}
    for path in paths:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                m = NODE.match(line)
                if m:
                    title, label = m.groups()
                    b = BYTES.search(label)
                    if b:
                        size, qualifier = int(b.group(1)), b.group(2)
                        old = defined.get(title)
                        if old is None or size > old[0]:
                            defined[title] = (size, qualifier, path)
                    continue
                m = EDGE.match(line)
                if m:
                    src, dst = m.groups()
                    callees = calls.setdefault(src, [])
                    if dst not in callees:
                        callees.append(dst)
    return defined, calls


def main(argv):
    parser = argparse.ArgumentParser(
        description="Worst-case stack of the controller's public functions "
                    "from -fcallgraph-info=su .ci files.")
    parser.add_argument("--limit", type=int, default=1024, metavar="BYTES",
                        help="maximum worst-case stack per root (default 1024)")
    parser.add_argument("ci_files", metavar="CI_FILE", nargs="+")
    args = parser.parse_args(argv)
    if args.limit <= 0:
        parser.error("--limit must be positive")

    try:
        defined, calls = read_graph(args.ci_files)
    except (OSError, UnicodeDecodeError) as e:
        print("stack_budget: %s" % e, file=sys.stderr)
        return 2

    errors = []
    not_counted = set()
    memo = {}          # function -> (bytes, path)
    reported = set()

    def worst(fn, stack):
        if fn in memo:
            return memo[fn]
        if fn in stack:
            cycle = stack[stack.index(fn):] + [fn]
            key = ("cycle", frozenset(cycle))
            if key not in reported:
                reported.add(key)
                errors.append("recursion: " + " -> ".join(short(f) for f in cycle))
            return None
        if fn == INDIRECT:
            caller = stack[-1] if stack else "?"
            if ("indirect", caller) not in reported:
                reported.add(("indirect", caller))
                errors.append("indirect call in '%s'" % caller)
            return None
        if fn not in defined:
            if fn in ALLOWED_EXTERNAL:
                not_counted.add(fn)
                memo[fn] = (0, [fn])
                return memo[fn]
            if ("undefined", fn) not in reported:
                reported.add(("undefined", fn))
                errors.append("'%s' is called (from '%s') but defined in no .ci file"
                              % (fn, stack[-1] if stack else "?"))
            return None
        size, qualifier, path = defined[fn]
        if qualifier != "static" and ("qualifier", fn) not in reported:
            reported.add(("qualifier", fn))
            errors.append("'%s' has a '%s' stack (%d bytes, %s)"
                          % (fn, qualifier, size, path))
        best = (0, [])
        ok = True
        for callee in calls.get(fn, []):
            r = worst(callee, stack + [fn])
            if r is None:
                ok = False
                continue
            if r[0] > best[0] or not best[1]:
                best = r
        result = (size + best[0], [fn] + best[1])
        if ok:
            memo[fn] = result
        return result

    results = []
    for root in ROOTS:
        if root not in defined:
            errors.append("root '%s' is defined in no .ci file" % root)
            continue
        r = worst(root, [])
        results.append((root, r))

    width = max(len(r) for r in ROOTS)
    for root, r in results:
        if r is None:
            continue
        size, path = r
        print("%-*s  %4d bytes  (worst path: %s)"
              % (width, root, size, " -> ".join(short(f) for f in path)))
        if size > args.limit:
            errors.append("'%s' needs %d bytes, above the limit of %d"
                          % (root, size, args.limit))
    print("not counted (C library): %s"
          % (", ".join(sorted(not_counted)) or "none"))
    for e in errors:
        print("FAIL: " + e)
    if errors:
        print("stack budget FAIL")
        return 1
    print("stack budget OK (limit %d bytes)" % args.limit)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

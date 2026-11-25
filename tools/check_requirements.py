#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Traceability check for the safety requirements (docs/safety.md).

Every requirement SR-n defined in docs/safety.md must be checked by at least
one acceptance criterion or one unit test, and every criterion or test that
cites a requirement must cite one that exists.

- A requirement is defined by a heading that starts with "### SR-n:".
- An acceptance criterion cites requirements at the start of its text (the
  field after the fifth comma of a row in scenarios/*.accept.csv):
  "SR-4: ..." or "SR-4, SR-7: ...".
- A unit test cites them with a comment in tests/*.c: /* SR-4 */ or
  /* SR-4, SR-7 */.

Prints which criteria and tests check each requirement.

Usage: tools/check_requirements.py [repository root]
Exit:  0 every requirement is checked and every citation exists; 1 otherwise.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parent.parent)

DEFINITION = re.compile(r"^### (SR-\d+):")
CRITERION_PREFIX = re.compile(r"^(SR-\d+(?:, SR-\d+)*): ")
TEST_COMMENT = re.compile(r"/\* (SR-\d+(?:, SR-\d+)*) \*/")
TEST_FUNCTION = re.compile(r"^static void (test_\w+)\(void\)")
ANY_SR = re.compile(r"SR-\d+")


def requirements():
    defined = []
    for line in (ROOT / "docs/safety.md").read_text().splitlines():
        m = DEFINITION.match(line)
        if m:
            defined.append(m.group(1))
    return defined


def criteria():
    """(where, [SR ids]) for each criterion, and the rows that mention SR- without the prefix format."""
    cited, malformed = [], []
    for path in sorted((ROOT / "scenarios").glob("*.accept.csv")):
        for number, line in enumerate(path.read_text().splitlines(), 1):
            if not line or line.startswith("#") or line.startswith("id,"):
                continue
            fields = line.split(",", 5)
            if len(fields) < 6:
                continue
            where = f"{path.stem.removesuffix('.accept')}:{fields[0]}"
            m = CRITERION_PREFIX.match(fields[5])
            if m:
                cited.append((where, m.group(1).split(", ")))
            elif ANY_SR.search(fields[5]):
                malformed.append(f"{path.relative_to(ROOT)}:{number}")
    return cited, malformed


def tests():
    """(where, [SR ids]) for each tagged test: the comment's file and the next test function."""
    cited = []
    for path in sorted((ROOT / "tests").glob("*.c")):
        lines = path.read_text().splitlines()
        for number, line in enumerate(lines):
            m = TEST_COMMENT.search(line)
            if not m:
                continue
            name = next((f.group(1) for f in map(TEST_FUNCTION.match, lines[number:number + 5]) if f), None)
            where = f"{path.stem}:{name}" if name else f"{path.relative_to(ROOT)}:{number + 1}"
            cited.append((where, m.group(1).split(", ")))
    return cited


def main():
    defined = requirements()
    by_criteria, malformed = criteria()
    by_tests = tests()
    problems = []

    if not defined:
        problems.append("docs/safety.md defines no requirement (### SR-n: ...)")
    duplicates = sorted({sr for sr in defined if defined.count(sr) > 1})
    problems += [f"{sr} is defined more than once in docs/safety.md" for sr in duplicates]
    problems += [f"{where}: SR- in the text, but not as the prefix 'SR-n: '" for where in malformed]

    for where, ids in by_criteria + by_tests:
        problems += [f"{where} cites {sr}, which docs/safety.md does not define" for sr in ids if sr not in defined]

    for sr in defined:
        crit = [w for w, ids in by_criteria if sr in ids]
        unit = [w for w, ids in by_tests if sr in ids]
        print(f"{sr}: {len(crit)} criteria, {len(unit)} unit tests")
        for w in crit + unit:
            print(f"    {w}")
        if not crit and not unit:
            problems.append(f"{sr} is checked by no criterion and no unit test")

    print()
    for p in problems:
        print(f"FAIL  {p}")
    print(f"{len(defined)} requirements, {len(by_criteria)} criteria and {len(by_tests)} unit tests cite them, "
          f"{len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

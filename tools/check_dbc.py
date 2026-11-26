#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that open-crank.dbc agrees with open-crank's encoders.

    oc-dbc-vectors | tools/check_dbc.py open-crank.dbc

oc-dbc-vectors (tests/dbc_vectors.c) encodes frames with the code that
really puts them on the bus and prints what open-crank means by each field.
This script decodes the same frames with the DBC (cantools) and requires:

- every frame to match a DBC message by J1939 PGN (and source address, so
  that the bench-only frames are told apart), with the right length;
- every signal open-crank printed to be in the DBC and to decode to the same
  value, within half a step of its scaling;
- every DBC signal of a matched frame to be checked by some vector;
- every DBC message to have at least one vector;
- every period the bench and the controller use to be the message's
  GenMsgCycleTime;
- the database to say ProtocolType "J1939" and each message VFrameFormat
  J1939PG.

Exit 0 when all agree, 1 on any mismatch, 2 on a usage or input error.
Needs cantools (tools/requirements-dbc.txt). Exit 77 (skipped) without it,
for ctest.
"""
import sys

try:
    import cantools
    from cantools.j1939 import frame_id_unpack, pgn_pack
except ImportError:
    print("SKIP  cantools is not installed (pip install -r tools/requirements-dbc.txt)")
    sys.exit(77)


def pgn_and_sa(frame_id):
    u = frame_id_unpack(frame_id & 0x1FFFFFFF)
    pdu_specific = u.pdu_specific if u.pdu_format >= 240 else 0
    pgn = pgn_pack(u.reserved, u.data_page, u.pdu_format, pdu_specific)
    return pgn, u.source_address


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: oc-dbc-vectors | tools/check_dbc.py <file.dbc>", file=sys.stderr)
        return 2
    try:
        db = cantools.database.load_file(sys.argv[1])
    except Exception as e:  # cantools raises several kinds; any of them means a broken DBC
        print(f"MISMATCH  {sys.argv[1]} does not load: {e}")
        return 1
    problems = []

    proto = db.dbc.attributes.get("ProtocolType")
    if proto is None or proto.value != "J1939":
        problems.append('database: ProtocolType is not "J1939"')
    by_key = {}
    for m in db.messages:
        fmt = m.dbc.attributes.get("VFrameFormat")
        fmt_value = fmt.value if fmt is not None else db.dbc.attribute_definitions["VFrameFormat"].default_value
        if fmt_value not in (3, "J1939PG"):
            problems.append(f"{m.name}: VFrameFormat is not J1939PG")
        by_key[pgn_and_sa(m.frame_id)] = m

    covered = set()
    checked_signals = 0
    lines = [l.strip() for l in sys.stdin if l.strip()]
    if not lines:
        print("check_dbc: no vectors on stdin", file=sys.stderr)
        return 2
    for line in lines:
        fields = line.split(",")
        if fields[0] == "cycle":
            name, ms = fields[1], int(fields[2])
            try:
                m = db.get_message_by_name(name)
            except KeyError:
                problems.append(f"cycle {name}: no such message in the DBC")
                continue
            if m.cycle_time != ms:
                problems.append(f"{name}: GenMsgCycleTime {m.cycle_time} ms, the code sends every {ms} ms")
            continue
        if fields[0] != "frame" or len(fields) < 4:
            print(f"check_dbc: bad line: {line}", file=sys.stderr)
            return 2
        name, frame_id, data = fields[1], int(fields[2], 16), bytes.fromhex(fields[3])
        expect = {}
        for kv in fields[4:]:
            k, v = kv.split("=", 1)
            expect[k] = int(v) if v.isdigit() else float(v)
        m = by_key.get(pgn_and_sa(frame_id))
        if m is None:
            problems.append(f"{name} {frame_id:08X}: no DBC message with its PGN and source address")
            continue
        if m.name != name:
            problems.append(f"{name} {frame_id:08X}: the DBC calls it {m.name}")
        if m.length != len(data):
            problems.append(f"{name}: DBC length {m.length}, frame length {len(data)}")
            continue
        covered.add(m.name)
        decoded = m.decode(data, decode_choices=False, scaling=True)
        for s in m.signals:
            if s.name not in expect:
                problems.append(f"{name}: DBC signal {s.name} is not checked by any vector")
        for k, v in expect.items():
            if k not in decoded:
                problems.append(f"{name}: open-crank's {k} is not in the DBC")
                continue
            s = m.get_signal_by_name(k)
            tol = abs(s.scale) / 2.0 if isinstance(v, float) else 0
            got = decoded[k]
            if abs(got - v) > tol:
                problems.append(f"{name} {data.hex().upper()}: {k} = {got} by the DBC, {v} by open-crank")
            checked_signals += 1

    for m in db.messages:
        if m.name not in covered:
            problems.append(f"{m.name}: no vector for this DBC message")

    for p in problems:
        print("MISMATCH  " + p)
    print(f"check_dbc: {len(db.messages)} messages, {checked_signals} signal values checked, "
          f"{len(problems)} mismatches")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())

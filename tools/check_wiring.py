#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the Level 2 wiring diagrams against the wiring table and the Tang's pins.

Usage:
  tools/check_wiring.py [--svg]

For every signal (crank, cam, inj1-6, analog1-4, GND) it compares:
  - platforms/stm32g4/wiring-l2.yml and wiring-l2-analog.yml (the WireViz
    sources): the Tang header and position, the Tang pin, the NUCLEO
    connector-pin and the MCU pin;
  - platforms/stm32g4/WIRING-L2.md: the pin map and the header positions;
  - open-tick's gateware/tang_nano_20k/tang_nano_20k.cst: the Tang pin.
A signal missing from any of them, or one value that differs, fails.

--svg also checks that each committed .svg is the render of its committed .gv:
the same nodes and edges, and every text of the .gv in it. Not its bytes:
Graphviz's output depends on the machine's fonts (tools/wireviz.sh).

Needs PyYAML (tools/wireviz.sh runs it in its venv). Exit 0 ok, 1 a mismatch.
"""

import html
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent
BOARD = ROOT / "platforms" / "stm32g4"
YMLS = [BOARD / "wiring-l2.yml", BOARD / "wiring-l2-analog.yml"]
MD = BOARD / "WIRING-L2.md"
CST = ROOT / "external" / "open-tick" / "gateware" / "tang_nano_20k" / "tang_nano_20k.cst"

TANG = {"TangL": "left", "TangR": "right"}
NUCLEO = ("CN3", "CN4")


def signal_key(text):
    """'Crank, 60-2 wheel' -> crank, 'Injector 3' -> inj3, 'analog 2' -> analog2, 'Ground' -> GND."""
    t = text.strip().lower()
    if t.startswith("crank"):
        return "crank"
    if t.startswith("cam"):
        return "cam"
    m = re.match(r"(injector|analog)\s+(\d)", t)
    if m:
        return ("inj" if m.group(1) == "injector" else "analog") + m.group(2)
    if t.startswith("ground"):
        return "GND"
    return None


def from_md():
    """{signal: {tang_pin, nucleo, mcu, header, position}} from WIRING-L2.md."""
    out = {}
    for line in MD.read_text(encoding="utf-8").splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) == 8:  # the pin map
            key = signal_key(cells[0])
            if key is None:
                continue
            row = out.setdefault(key, {})
            if key != "GND":
                row["tang_pin"] = cells[1]
            m = re.match(r"(CN\d)-(\d+)", cells[3])
            if m:
                row["nucleo"] = f"{m.group(1)}-{m.group(2)}"
            if cells[4]:
                row["mcu"] = cells[4]
        elif len(cells) == 4 and cells[0] in ("left", "right"):  # header positions
            key = signal_key(cells[3].replace("ground: position 20; 3V3 is not used", "ground"))
            if key is None:
                continue
            row = out.setdefault(key, {})
            pos = cells[1].split(",")[-1].strip() if key == "GND" else cells[1]
            row["header"] = f"{cells[0]} {pos}"
            if key != "GND":
                row["tang_pin_header"] = cells[2].split()[0]
    return out


def from_cst():
    out = {}
    for m in re.finditer(r'IO_LOC "(crank|cam|inj\[(\d)\]|analog\[(\d)\])" (\d+);', CST.read_text()):
        name = m.group(1)
        if m.group(2) is not None:
            name = f"inj{int(m.group(2)) + 1}"
        elif m.group(3) is not None:
            name = f"analog{int(m.group(3)) + 1}"
        out[name] = m.group(4)
    return out


def from_yml():
    """{signal: {tang_pin, header, nucleo, mcu}}, following each wire by its label."""
    out = {}
    for path in YMLS:
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        conns, cables = doc["connectors"], doc["cables"]

        def pin_label(conn, pin):
            c = conns[conn]
            return str(c["pinlabels"][[str(p) for p in c["pins"]].index(str(pin))])

        for chain in doc["connections"]:
            items = [list(x.items())[0] if isinstance(x, dict) else None for x in chain]
            ends = [it for it in items if it is not None and it[0] not in cables]
            cable = [it for it in items if it is not None and it[0] in cables][0]
            labels = cables[cable[0]]["wirelabels"]
            wires = []
            for w in cable[1]:
                if isinstance(w, str) and "-" in w:
                    a, b = (int(x) for x in w.split("-"))
                    wires += list(range(a, b + 1))
                else:
                    wires.append(int(w))
            for i, wire in enumerate(wires):
                sig = str(labels[wire - 1])
                if sig in ("3V3",):
                    continue
                row = out.setdefault(sig, {})
                for name, pins in ends:
                    pins = pins if isinstance(pins, list) else [pins]
                    pin = pins[i]
                    if name in TANG:
                        row["header"] = f"{TANG[name]} {pin}"
                        if sig != "GND":
                            row["tang_pin"] = pin_label(name, pin).split()[0]
                    elif name in NUCLEO:
                        row["nucleo"] = f"{name}-{pin}"
                        if sig != "GND":
                            row["mcu"] = pin_label(name, pin).split()[1]
                    elif name == "SUP" and sig == "GND":
                        row.setdefault("nucleo_supply", str(pin))
    return out


def check_tables():
    md, cst, yml = from_md(), from_cst(), from_yml()
    signals = ["crank", "cam"] + [f"inj{k}" for k in range(1, 7)] + [f"analog{k}" for k in range(1, 5)] + ["GND"]
    bad = 0
    for s in signals:
        m, y = md.get(s, {}), yml.get(s, {})
        problems = []
        for field in ("header", "nucleo", "mcu", "tang_pin"):
            if s == "GND" and field in ("mcu", "tang_pin"):
                continue
            if m.get(field) is None or y.get(field) is None:
                problems.append(f"{field}: table {m.get(field)!r}, diagram {y.get(field)!r}")
            elif m[field] != y[field]:
                problems.append(f"{field}: table {m[field]}, diagram {y[field]}")
        if s != "GND":
            if m.get("tang_pin_header") != m.get("tang_pin"):
                problems.append(f"Tang pin: pin map {m.get('tang_pin')}, header table {m.get('tang_pin_header')}")
            if cst.get(s) != m.get("tang_pin"):
                problems.append(f"Tang pin: table {m.get('tang_pin')}, .cst {cst.get(s)}")
        desc = f"{y.get('header', '?')} (Tang {y.get('tang_pin', '-')}) -> {y.get('nucleo', '?')} {y.get('mcu', '')}"
        print(f"{'ok  ' if not problems else 'FAIL'}  {s:<8} {desc}{'' if not problems else '  ' + '; '.join(problems)}")
        bad += bool(problems)
    return bad


def check_svg(stem):
    gv = (BOARD / f"{stem}.gv").read_text(encoding="utf-8")
    svg = (BOARD / f"{stem}.svg").read_text(encoding="utf-8")
    nodes = set(re.findall(r"^\t(\w+) \[", gv, re.M)) - {"graph", "node", "edge"}
    edges = len(re.findall(r" -- ", gv))
    svg_nodes = set(html.unescape(t) for t in re.findall(r'<g id="node\d+" class="node">\s*<title>([^<]*)</title>', svg))
    svg_edges = len(re.findall(r'class="edge"', svg))
    texts = {html.unescape(t).strip() for t in re.findall(r"<td[^>]*>([^<]+)</td>", gv) if t.strip()}
    svg_texts = {html.unescape(t).strip() for t in re.findall(r"<text[^>]*>([^<]*)</text>", svg)}
    missing = sorted(t for t in texts if t not in svg_texts)
    problems = []
    if nodes != svg_nodes:
        problems.append(f"nodes differ: {sorted(nodes ^ svg_nodes)}")
    if edges != svg_edges:
        problems.append(f"{edges} edges in the .gv, {svg_edges} in the .svg")
    if missing:
        problems.append(f"texts of the .gv not in the .svg: {missing[:5]}")
    print(f"{'ok  ' if not problems else 'FAIL'}  {stem}.svg is the render of {stem}.gv"
          f" ({len(nodes)} nodes, {edges} edges, {len(texts)} texts){'' if not problems else ': ' + '; '.join(problems)}")
    return bool(problems)


def main():
    bad = check_tables()
    if "--svg" in sys.argv[1:]:
        for y in YMLS:
            bad += check_svg(y.stem)
    print(f"check_wiring: {'every signal agrees' if not bad else f'{bad} problem(s)'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

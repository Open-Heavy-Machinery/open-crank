# Level 2 in a closed loop on the board (2026-11-28)

open-crank v0.4, Level 2 step 7: the Level 2 plant, the hoist pump and the reference machine controller on the host, against the reference engine ECU (`oc-stm32g4-ecu`) on a NUCLEO-G431KB, through open-tick's Tang Nano 20K bridge. How it works: [docs/acceptance.md, "On the board, in a closed loop"](../../acceptance.md#on-the-board-in-a-closed-loop).

**Run:** `tools/board/hilrun.py --hil build/open-crank-hil --bitstream <open-tick>/build-gateware/bridge.fs --out …`, with the defaults: every scenario of `scenarios/l2` the board can play (all but `crank_tooth_lost`, a fault of the in-memory edge model).

- open-crank commit `2f636ef19b21e72f394240c2528ff460e338cb30`, with open-tick at `b6840f0` (its release/v0.4.0 branch: the code of v0.4.0); bitstream md5 `6659c249a3a8c779c0f7bb9cac6e6fa5`, loaded fresh before each scenario; the NUCLEO reset before each.
- The Tang on a root USB port (`3-2`, 480 Mbit/s); CAN on the CANable 2 at 250 kbit/s (`can0`); the host about 92 % idle, no browser.
- Result: **every scenario passed every criterion (29 of 29)**. The first hard runs of `overspeed` and `start_to_idle` were `degraded` (3 underruns each, after a send that went out 1.0 and 1.5 ms late; every criterion passed in them too) and were retried: both runs are kept (`*.attempt2.*` is the retry).

| Scenario | Criteria | Hard run | Injections | Misplaced | First injection | Capture lag max | Start of injection p99 / max |
|---|---|---|---|---|---|---|---|
| `hoist_step` | 9 of 9 | ok | 1668 | 0 | 2.64 rev | 30 ms | 0.0158° / 0.0296° |
| `hold_1500` | 4 of 4 | ok | 928 | 0 | 2.64 rev | 29 ms | 0.0241° / 0.0296° |
| `hold_2100` | 4 of 4 | ok | 1237 | 0 | 2.64 rev | 29 ms | 0.0196° / 0.0301° |
| `hold_750` | 4 of 4 | ok | 491 | 0 | 2.64 rev | 29 ms | 0.0114° / 0.0140° |
| `overspeed` | 4 of 4 | ok on the retry | 271 | 0 | 2.31 rev | 29 ms | 0.0264° / 0.0288° |
| `start_to_idle` | 4 of 4 | ok on the retry | 379 | 0 | 2.64 rev | 29 ms | 0.0101° / 0.0126° |

- **Synchronises:** every injection in its own cylinder's window, on its own channel; the first one within 2.31–2.64 revolutions from rest; no misfire once running.
- **Holds a requested speed** (the plant, from 6 s): 747.8–748.2 rpm for 750, 1496.1–1496.7 for 1500, 2093.8–2096.6 for 2100. The ECU holds what it reads with its HSI (0.27 % fast), so the plant runs 0.27 % low, as in memory.
- **`hoist_step`:** the dip under the 570 Nm step to 1267.9 rpm (limit 1207), back within ±0.5 % 1.70 s after it; the overshoot on release to 1787.5 rpm (limit 1865), back 0.99 s after it.
- **`overspeed`:** the external torque drives the plant to 3179 rpm; the ECU's trip keeps the fuel cut until it stops.
- **The jitter budget** (p99 ≤ 0.1°, max ≤ 0.5°): the worst p99 is 0.0264°, the worst max 0.0301°, against the angle each injection was commanded at, measured from the bridge's own accumulator. The widths are not judged here (the host does not know the width the ECU commanded): they were measured at constant speed with a known width ([2026-11-28-ecu-board/](../2026-11-28-ecu-board/)).
- **The hard runs kept as the result:** 0 underruns; round trip p99 1.91–1.95 ms (budget 2.2 ms); clock mapping bound 0.66–0.73 ms (1 ms); CAN send error p99 0.58–0.63 ms (1.1 ms); DUT-to-plant delay 30.9 ms (31.2 ms); the captures' lag 29–30 ms (31.2 ms).

| File | What |
|---|---|
| `summary.txt`, `hilrun_stdout.txt` | the script's table and its whole output |
| `meta.txt` | commit, bitstream, the bridge's fidelity file, the Tang's USB path |
| `<scenario>.txt` | `open-crank-hil`'s output: the fidelity check, the hard run, the criteria |
| `<scenario>.trace.csv` | the plant every 10 ms |
| `<scenario>.frames.csv` | every CAN frame (TSC1, the controller's DM1, the ECU's EEC1) with its logical time and its bus time on the bridge's clock |
| `<scenario>.verdicts.csv` | the oracle's verdicts |
| `<scenario>.jitter.csv` | the jitter budget (start of injection) |
| `<scenario>.budget.csv` | open-tick's budget of the hard run |
| `<scenario>.usart2.log` | the ECU's own USART2 rows during the run |

`md5sum -c md5.txt` checks the files.

**Known limits, seen the same day:**
- **A hard run at 1 ms steps with H = 16 is `degraded` now and then** (2 of 8 here, 3 underruns each): when the host's sender wakes about 1 ms late, the reply misses the BL616's forwarding instant and the next exchange finds the buffer nearly empty. The plant and the criteria were not affected in any of them. A larger H, or a real-time priority for the sender (`SCHED_FIFO`, a deferred improvement), would leave more room.
- **The Tang's BL616 resets its USB about every 15 minutes of running** (the kernel logs `usb 3-2: USB disconnect`). A hard run under way then ends `aborted` with `abort=io` in its budget. It happened once during this campaign (19:52:35), and no run of it was aborted; it happened nine times that day. The cause is not known.
- Earlier runs that day, on the commits before this one, showed the three link problems fixed in open-tick v0.4.0 (the first exchange after the start, the margin, the declared delay) and are not kept here.

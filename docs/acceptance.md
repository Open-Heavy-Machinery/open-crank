# Acceptance suite

The acceptance suite grades **any** device under test (DUT) by the CAN frames it sends, and by nothing else. It does not know how the DUT is implemented: the reference controller (`open-crank-mc`), your own firmware on a board and a program on your laptop are graded the same way.

`open-crank-accept` plays a scenario with the simulated machine, as `open-crank-sil` does, and an oracle checks the DUT's frames against criteria written for that scenario. It prints one line per criterion and a verdict.

## What it grades, and what it does not

It grades what the DUT asks of the engine (TSC1) and of the hoist pump (OC_PUMP_CMD), and when, measured from the events of the scenario.

It does not grade:

- **The kind of fault or the internal state.** A controller that releases the override because it lost EEC1 and one that releases it for any other reason look the same on the bus. The suite sees `tsc1_override == 0`, not `FAULT: engine status lost`.
- **The timing of a real network.** Times are measured on `vcan` or with the board on a two-node bench, not on a vehicle network with other traffic.
- **The ramp.** The reference controller slews its requested speed; the slew is not a requirement, and a DUT that jumps straight to the target passes.

## Observables

The oracle reads seven observables from the DUT's frames. Only frames whose source address is the DUT's count (`--dut`, `0x27` by default); a TSC1 counts only if it is addressed to the engine (`0x00`). Engine frames are never looked at.

| Observable | Value | Holds for | Default |
|---|---|---|---|
| `tsc1_speed_control` | 1 if the TSC1 asks for speed control (override mode `01`) with a valid requested speed; 0 otherwise | 200 ms | 0 |
| `tsc1_speed_rpm` | the requested speed of that TSC1 while `tsc1_speed_control` is 1; a gap otherwise | 200 ms | gap |
| `pump_cmd_pct` | the OC_PUMP_CMD displacement; 0 if "not available" | 200 ms | 0 |
| `tsc1_override` | 1 if the TSC1 has an override mode other than `00` (speed, torque or limit); 0 if it is `00` | 200 ms | 0 |
| `tsc1_intact` | 1 if the TSC1's checksum (SPN 4207) is right and its counter (SPN 4206) is the previous TSC1's + 1, with 15 wrapping to 0; 0 otherwise. The first TSC1 needs only the checksum (since v0.3.1) | 200 ms | 1: silence is not a broken frame |
| `dm1_red_stop` | 1 if the DUT's own DM1 (single frame) has its red stop lamp on; 0 otherwise | 3000 ms | 0 |
| `dm1_spn` | the SPN of the first active DTC of the DUT's own DM1; a gap when it has none | 3000 ms | gap |

The two DM1 observables hold for 3000 ms, three periods of a DM1 sent once a second: the 200 ms of the others would make them flicker between frames. A DUT that never sends DM1 reads as "no lamp, no code" (SR-14).

**Silence counts as releasing.** An observable falls back to its default when no frame has updated it for more than 200 ms, because that is what the simulated machine does: the engine drops a TSC1 older than 200 ms and returns to its own idle governor, and the pump takes a command older than 200 ms as zero (and "not available" as zero). "Released" therefore means: TSC1 with override mode `00`, or no TSC1 for more than 200 ms; and the pump at 0, or no OC_PUMP_CMD for more than 200 ms. A TSC1 that keeps any other override mode is not a release, even without a valid requested speed: the simulated engine then falls back to low idle, but the bus contract does not say what a real engine does with a speed-control request that carries no speed, and a release in mode `00` is unambiguous.

A term on a gap is false: `tsc1_speed_rpm >= 1499` does not hold while the DUT is not asking for a speed.

## Criteria files

The format is open-tick's ([docs/criteria-format.md](https://github.com/Open-Heavy-Machinery/open-tick/blob/v0.2.0/docs/criteria-format.md), since `v0.2.0`); this section summarises it with open-crank's 10 ms step.

Each scenario's criteria live next to it: `scenarios/<name>.accept.csv` for `scenarios/<name>.csv`.

```
# Acceptance criteria for scenarios/hoist_step.csv. Format: docs/acceptance.md.
id,from,until,tolerance_ms,condition,text
S2,start=1+2000ms,end,0,tsc1_speed_control == 1,speed control taken within 2 s of start and kept
H2,hoist_demand_pct=85+100ms,hoist_demand_pct=0-10ms,0,pump_cmd_pct >= 84.6 && pump_cmd_pct <= 85.4,pump follows the hoist lever (85 %) within 100 ms
```

- Lines starting with `#` and blank lines are ignored. The first other line is the header, exactly `id,from,until,tolerance_ms,condition,text`.
- There is no quoting. `text` is everything after the fifth comma, so it may contain commas.
- In open-crank, a criterion that checks a safety requirement starts its `text` with the requirement's number, or numbers: `SR-2: requested speed never above high idle (2100 rpm)`, `SR-4, SR-7: override released and pump at 0 …`. The requirements are in [safety.md](safety.md), and `tools/check_requirements.py` checks that each one is cited and that every number cited exists. To the oracle it is ordinary text.
- `id`: letters, digits, `_`, `.` or `-`, unique in the file.
- `from` and `until` bound the window, **inclusive**. Each is a reference with an optional offset `+Nms` or `-Nms`:
  - `begin`: t = 0;
  - `end`: the scenario's `end` event;
  - a time in seconds, such as `5.0`;
  - `<signal>=<value>`: the **first** event of the scenario that sets that signal to that value, such as `start=1` or `fault.engine_tx_lost=1`.
- `tolerance_ms`: whole milliseconds.
- `condition`: `term (&& term)*`, optionally followed by `-> term (&& term)*` (implication: if the left side holds, the right side must hold). A term is `observable op number`, with `op` one of `== != < <= > >=`.

**Evaluation.** At every step `t` inside the window (every 10 ms), with the values after the frames received in that step and the defaults of the stale observables, the condition is evaluated. The criterion **fails** if the condition is false at consecutive steps that cover **more** than `tolerance_ms`; each step covers 10 ms. With 0, one false step fails; with 20, two consecutive false steps pass and three fail. A window the run did not reach to its end fails.

**Errors.** A wrong header, an unknown observable or signal, an event the scenario does not have, an invalid operator, a window that starts before t = 0, ends after `end` or is empty, are format errors: the runner stops with the line number and exit code 2 before the run starts. Limits: 64 criteria per file, 8 terms per condition, lines of 511 characters.

**Report.** One line per criterion: `PASS` or `FAIL`, the id, the text and the window in seconds. A failure adds the first instant of the violation and the values of the condition's observables then. When `from` is an event plus an offset, the line also gives the **reaction time**: how long after the event the condition had been holding without interruption up to `from`.

```
PASS  S2  speed control taken within 2 s of start and kept  [2.50 s .. 25.00 s]  reaction: 990 ms after start=1
FAIL  E2  override released and pump at 0 within 300 ms of losing EEC1, and kept  [8.30 s .. 14.00 s]  reaction: not holding at 8.30 s  first violation at 8.30 s: tsc1_override = 1, pump_cmd_pct = 50
```

## The criteria

Five criteria are common to the fifteen scenarios (the requirement each checks is in [safety.md](safety.md)):

| Id | Window | Condition | Why |
|---|---|---|---|
| S1 | from the start of the run to 10 ms before the starter is pressed | no override | the DUT must not request an override before the operator starts the engine |
| S2 | from 2 s after the starter is pressed to `end`, or to 10 ms before the scenario's fault (in `target_above_high_idle`, before the request above high idle) | speed control | 2 s covers cranking and the reference's 500 ms wait for a running engine (EEC1 above 600 rpm), with margin. Not in `override_ignored`, where the engine never honours the request |
| G1 | the whole run | speed control implies a requested speed of at most 2100 rpm | the engine's high idle; above it the request means nothing |
| G2 | the whole run, 20 ms tolerance | pump above 0 implies speed control | the pump loads the engine only while the DUT holds its speed; 20 ms because TSC1 and OC_PUMP_CMD are two frames that need not change in the same step |
| G3 | the whole run | `tsc1_intact == 1` | every TSC1 must carry a valid checksum and a counter that moves by one, so that the engine can refuse a corrupted or repeated request (SR-10); the engine refuses one that does not |

And per scenario:

| Scenario | Id | Window starts | Condition | Why T |
|---|---|---|---|---|
| `cold_start` | C1 | 5.0 s | requested speed 1500 ± 1 rpm | the operator's target; 5 s leaves time for the reference's slew (500 rpm/s from low idle) |
| `hoist_step` | H1 | 5.0 s | requested speed 1500 ± 1 rpm | as C1 |
| | H2 | 100 ms after the lever goes to 85 % | pump at 85 ± 0.4 % until the lever is released | 100 ms = OC_OPERATOR every 20 ms + one 10 ms step + margin; 0.4 % is one bit of OC_PUMP_CMD |
| | H3 | 100 ms after the lever is released | pump at 0 | as H2 |
| `engine_status_loss` | E1 | 100 ms after the lever goes to 50 % | pump at 50 ± 0.4 % until EEC1 is lost | as H2 |
| | E2 | 300 ms after EEC1 is lost | no override and pump at 0, kept | 300 ms = the 200 ms EEC1 timeout + margin |
| `override_ignored` | O1 | 2.5 s after the starter is pressed | no override and pump at 0, kept | 2.5 s covers S2's 2 s plus the reference's 500 ms wait for EEC1 to show its address |
| `overspeed` | V1 | 1 s after the overrunning load starts | no override and pump at 0, kept | the reference releases at its overspeed limit of 2300 rpm (the engine itself trips at 2400 rpm); 1 s depends on how long the plant takes to get there under 400 Nm of overrun |
| `speed_command_loss` | L1 | 1 s after the DUT's frames stop reaching the engine, until 10 ms before the fault reset | no override and pump at 0 | 1 s covers the engine's 200 ms TSC1 timeout and the reference's 500 ms wait for EEC1 to show its address, with margin. Until the reset, it also checks that the DUT does not take control again by itself |
| | L2 | 1 s after the fault reset | speed control, kept | the engine is already running: 1 s covers the reference's 500 ms wait for a running engine, with margin |
| | L3 | 18.0 s | requested speed 1500 ± 1 rpm | back at the operator's target once the slew has finished |
| `target_above_high_idle` | G1 | the whole run | speed control implies a requested speed of at most 2100 rpm | the operator asks for 2500 rpm at 8 s; the engine would clamp it to 2100 by itself, so only the DUT's own request shows whether it respects high idle |
| `engine_status_delay` | E1 | 100 ms after the lever goes to 50 % | pump at 50 ± 0.4 % until the delay starts | as H2 |
| | D1 | 300 ms after the engine's frames start arriving 300 ms late, until 10 ms before the fault reset | no override and pump at 0 | for the first 300 ms nothing arrives, so the reference times out as in E2; after that the frames flow again, only 300 ms old, and the fault must stay latched (SR-7) until the reset. 300 ms = the 200 ms EEC1 timeout + margin |
| | D2 | 1 s after the fault reset | speed control, kept | as L2; the delay is gone 500 ms before the reset |
| | D3 | 18.0 s | requested speed 1500 ± 1 rpm | as L3 |
| `engine_status_jitter` | J1 | when the jitter starts, to `end` | speed control at 1500 ± 1 rpm | up to 150 ms of jitter is below the 200 ms EEC1 timeout: a controller that faults on it fails. The order rule of the links ([open-tick `docs/link-faults.md`](https://github.com/Open-Heavy-Machinery/open-tick/blob/main/docs/link-faults.md)) keeps EEC1 in order, so no gap is longer than about 160 ms |
| | J2 | 100 ms after the lever goes to 50 %, to `end` | pump at 50 ± 0.4 % | as H2, also under jitter |
| `speed_command_intermittent` | I1 | when the loss starts, to `end` | speed control at 1500 ± 1 rpm | one frame in three lost leaves at most 20 ms between two TSC1 that arrive, far below the engine's 200 ms request timeout: the engine keeps honouring the override, and the controller must not fault |
| | I2 | 100 ms after the lever goes to 50 %, to `end` | pump at 50 ± 0.4 % | as H2, also while frames are lost |
| `mc_bus_off` | B1 | 1 s after the controller's direction goes bus-off, until 10 ms before the fault reset | no override and pump at 0 | as L1: the same loss, through the link's bus-off |
| | B2 | 1 s after the fault reset | speed control, kept | as L2 |
| | B3 | 18.0 s | requested speed 1500 ± 1 rpm | as L3 |
| `speed_command_corrupt` | K1 | 1 s after the controller's frames start arriving with byte 3 flipped, until 10 ms before the fault reset | no override and pump at 0 | as L1: the engine refuses every corrupted TSC1 by its checksum, as if it were lost |
| | K2 | 1 s after the fault reset | speed control, kept | as L2 |
| | K3 | 18.0 s | requested speed 1500 ± 1 rpm | as L3 |
| `engine_dtc_red` | E1 | 100 ms after the lever goes to 50 % | pump at 50 ± 0.4 % until the lamp | as H2 |
| | R1 | 300 ms after the engine's red stop lamp, until 10 ms before the fault reset | no override and pump at 0 | the engine sends DM1 at once when it changes; the reference reacts in 10 ms. Kept after the lamp goes off (SR-7) |
| | R2 | 1 s after the fault reset | speed control, kept | as L2 |
| | R3 | 18.0 s | requested speed 1500 ± 1 rpm | as L3 |
| `engine_dtc_amber` | E1 | 100 ms after the lever goes to 85 % | pump at 85 ± 0.4 % until the lamp | as H2 |
| | A1 | 1 s after the amber lamp, until 10 ms before it goes off | requested speed 1200 ± 1 rpm and pump 50 ± 0.4 % | limp-home (SR-13). 1 s: the slew limit takes 0.6 s from 1500 to 1200 rpm. S2 runs to `end`: limp-home is not a fault |
| | A2 | from the amber lamp, for 1 s | requested speed ≥ 1199 rpm and pump ≥ 49.6 % | into limp-home without falling through it. The smoothness itself (rates) is checked by the unit test `test_limp_home`: the oracle has no rates |
| | A3 | 1.5 s after the lamp goes off, to `end` | requested speed 1500 ± 1 rpm and pump 85 ± 0.4 % | back to the operator's values on its own |
| `engine_dtc_red_and_amber` | M1 | 300 ms after both lamps, to `end` | no override and pump at 0 | two codes make the DM1 10 bytes long, so it comes as a BAM (TP.CM and two TP.DT, 50 ms apart): the reference reacts in 110 ms |
| every scenario that ends in FAULT (nine; not `watchdog_reset`, which is board-only) | F1 | 1.1 s after the bound of that scenario's release criterion (R1, M1, D1, E2, B1, O1, V1, K1, L1), until 10 ms before the fault reset or to `end` | the DUT's own DM1 with its red stop lamp and SPN 520192 + its fault number | SR-14. The release criterion bounds when the DUT is in FAULT at the latest; a DM1 sent once a second then needs up to 1 s, plus a step. The SPN tells which fault: 520193 engine status lost, 520194 overspeed, 520196 override rejected, 520198 the engine's red stop lamp ([bus-interface.md](bus-interface.md)) |
| the five of them with a fault reset | F2 | 1.1 s after the fault reset, to `end` | the DUT's own DM1 without its red stop lamp | SR-14: the report ends with the fault |

## Running it

### On `vcan`

```sh
sudo modprobe vcan
sudo ip link add dev vcan0 type vcan && sudo ip link set up vcan0
cmake -S . -B build && cmake --build build
tools/accept_vcan.sh build vcan0
```

`tools/accept_vcan.sh` runs every `scenarios/*.accept.csv`: for each, it starts a fresh `open-crank-mc` (a new process is a reset), waits 0.5 s, runs `open-crank-accept` with the scenario, and stops the controller. It prints each report, keeps them in `build/accept/`, and ends with one line per scenario (`PASS`, `FAIL` or `INVALID`). It exits with 0 if every scenario passes, 1 if any fails, and 3 if any run is `INVALID` and none fails. CI runs it in the `vcan-demo` job when the runner provides `vcan`.

A scenario whose run is INVALID (late steps, so it has no verdict) is run once more with a fresh controller, and only the second attempt counts. The first attempt stays in `<scenario>.attempt1.log`, both are printed, and the summary line says so. A FAIL is never run again: a failure is a verdict. This keeps a slow CI runner from turning a run without a verdict into a red build, without hiding any failure.

To grade your own controller, start it instead of `open-crank-mc` and run one scenario at a time:

```sh
your-controller vcan0 &
./build/open-crank-accept vcan0 scenarios/engine_status_loss.csv
```

### On a board

With the board on the bus (see [platforms/stm32g4/README.md](../platforms/stm32g4/README.md)), **reset the board before each scenario**: a controller that faulted stays in FAULT until the operator's fault reset, and the next scenario would start with the fault latched (S1 and S2 then fail, and the runner reminds you to reset).

```sh
# reset the board, then:
./build/open-crank-accept can0 scenarios/hoist_step.csv --dut 0x27
```

### Usage

```
open-crank-accept <can-interface> <scenario.csv> [--criteria <file>] [--dut <address>] [--trace <out.csv>]
                  [--verdicts <out.csv>] [--frames <out.csv>]
```

- `--criteria`: by default, the scenario's path with `.csv` replaced by `.accept.csv`.
- `--dut`: the DUT's source address, `0x27` by default.
- `--trace`: the same CSV trace as `open-crank-sil` writes.
- `--verdicts`: the oracle's verdicts, one row per criterion: verdict, status, window, event, reaction and first violation, in seconds on the trace's time axis. The format is open-tick's ([docs/verdict-format.md](https://github.com/Open-Heavy-Machinery/open-tick/blob/v0.2.1/docs/verdict-format.md)). Open it in [open-rig](https://github.com/Open-Heavy-Machinery/open-rig) next to the trace to see each window, reaction and failure over the signals. It is written from the same oracle state as the report, so the two always agree.
- `--frames`: every frame of the bus, with its logical and its bus time (see "Frame logs" below).

Every received frame goes to the oracle before it goes to the simulated machine, so the oracle sees what the DUT sends even while `fault.mc_tx_lost` or the `mc` link keeps it from the engine (see "Link faults" below). The run ends when the scenario reaches `end`; the last line is one of:

```
open-crank-accept: PASS (N of N criteria), 0 late steps
open-crank-accept: FAIL (K of N criteria failed), L late steps
open-crank-accept: INVALID: L late steps; the timing of this run cannot be trusted
```

If no frame from the DUT arrived, the runner says so before that line. If S1 or S2 failed, it reminds you to reset a board before each run.

| Exit code | Meaning |
|---|---|
| 0 | pass |
| 1 | fail |
| 2 | usage, format, I/O or Ctrl+C, or a link was full (no verdict; see "Link faults") |
| 3 | `INVALID`: some step was late, whatever the verdict |

A late step is a step that started after its wall-clock deadline: the simulated machine then ran slower than real time, so the DUT saw different timing from the one the criteria assume. Such a run is not trusted, even if every criterion passed.

### In memory, with the reference controller

```
open-crank-accept-loop <scenario.csv> [--criteria <file>] [--verdicts <out.csv>] [--frames <out.csv>]
```

`open-crank-accept-loop` grades only the reference controller (`MC_DEFAULT_PARAMS`, `0x27`), or a mutant built from it, with the same oracle and criteria as `open-crank-accept`, but in the closed loop of `sim/runtime/loop.c`: no bus, no wall clock, as fast as the CPU allows (a fraction of a second per scenario). `--criteria` defaults to the same file as for `open-crank-accept`, and `--verdicts` writes the same verdict file. It prints the same report and ends with `open-crank-accept-loop: PASS (N of N criteria)` or `open-crank-accept-loop: FAIL (K of N criteria failed)`; it exits with 0 if the run passes, 1 if it fails, and 2 on a usage, format or I/O error. It builds on any host, Linux or not.

```sh
./build/open-crank-accept-loop scenarios/engine_status_loss.csv
```

`ctest` runs it on every scenario that has criteria, fifteen, as the tests `accept_<scenario>` (`accept_cold_start`, `accept_engine_status_delay`, …). Each writes its verdict file into the build tree, and `accept_verdicts_hoist_step` compares hoist_step's with [tests/data/hoist_step.verdicts.csv](../tests/data/hoist_step.verdicts.csv), byte for byte.

### Frame logs (`--frames`)

`open-crank-accept`, `open-crank-accept-loop`, `open-crank-sil` and `open-crank-run` take `--frames <file>`. It writes every frame of the run, one row each, in open-tick's frame log format ([docs/frame-log-format.md](https://github.com/Open-Heavy-Machinery/open-tick/blob/v0.3.1/docs/frame-log-format.md)). Each row has:
- `t_s`, the step in which the frame crossed the simulated machine's side; it places the frame on the trace's time axis;
- `t_bus_s`, when it was on the bus, measured;
- its direction (`to_dut`, `from_dut`);
- the link that carried it (`engine`, `mc`, or empty for the operator panel and the test frame);
- what the link did to it (`drop`, `drop_one_in`, `bus_off`, `overflow`, `corrupt`). `fault.mc_tx_lost` shows as `drop` with no link: it is the bench's own drop, before the `mc` link.

Without `--frames`, nothing changes: the runners take the same code path as before, and the traces and verdicts are byte-identical either way (CI `traces`).

**The two times, and where `t_bus_s` comes from:**

| Runner | `bus_clock` | `t_bus_s` |
|---|---|---|
| `open-crank-accept-loop`, `open-crank-run` | `sim` | simulated: when the controller sent the frame, or received it. Exact, so `t_bus_s − t_s` of a delayed engine frame is the delay exactly. Two runs give the same file (`ctest`: `accept_frames_deterministic`, `run_frames_hoist_step`), and both runners give the same frames for a scenario, header's `source` aside (`run_frames_same_as_accept_loop`) |
| `open-crank-accept`, `open-crank-sil` | `kernel` | the kernel's timestamps, minus an anchor read at t = 0: received frames as they arrived, sent frames when they left on the interface (the socket's own copy, `CAN_RAW_RECV_OWN_MSGS`). On a USB adapter that is when the frame reached or left the PC, the adapter's latency included |

**What `t_bus_s − t_s` shows on a paced run.** The paced runners compute each step at the start of its 10 ms: when the pacer wakes at T, the bench runs the step from T to T + 10 ms and sends at once the frames that report it, stamped `t_s` = T + 10 ms; then it sleeps until T + 10 ms. So **a machine frame is on the bus about one step before its `t_s`**: measured on `vcan` (2026-11-26, `cold_start`), `t_bus_s − t_s` = −9.9 ms (p50) for `to_dut` frames. During a 300 ms engine-link delay it is 290 ms: the delay minus that step. A controller frame is on the bus before the step that reads it: −2.5 ms (p50) for `from_dut` on `vcan`, depending on the controller's phase. This is how the paced loop works, not a transport delay. A viewer shows `t_bus_s` as a measurement beside the frame, never as its place on the axis.

`tools/frames_vcan_check.sh` (CI, `vcan` job) runs `cold_start` with the reference controller and requires `bus_clock=kernel` and a bus time on every frame, so that every frame the bench sent was matched with its own copy.

## Level 2: the engine ECU (v0.4)

At Level 2 the device under test is the engine's own ECU: it reads the crank and the cam and drives six injectors. In memory, `open-crank-l2-run` runs it against the Level 2 plant (`sim/l2/l2_loop.h`): the reference ECU's logic (`portable/engine_ecu`), as it runs on the board, the plant at 1 ms with the bridge's own crank angle, the hoist pump, and the reference machine controller sending TSC1 and reading the ECU's EEC1.

```sh
build/open-crank-l2-run scenarios/l2/hoist_step.csv --trace hoist_l2.csv --verdicts hoist_l2.verdicts.csv --jitter hoist_l2.jitter.csv
```

- **The bridge, modelled.** The crank and cam edges are those the Tang Nano 20K's gateware puts out for the plant's accumulator, each at its exact tick, converted to the ECU's 170 MHz timer with the reference board's HSI bias (−2658 ppm, so the ECU reads 0.27 % fast). An injection the ECU arms starts on the next crank edge it sees, plus its delay; it reaches the plant as a capture 30 steps after it ends: the delay the board's captures had on the hard runs (H = 16 steps of look-ahead, up to two of the BL616's forwarding cycles, and a round trip; open-tick's bound is 31.2 ms).
- **Criteria on the engine.** The criteria are about what the engine does, not only what crosses the bus, so the runner hands the oracle the plant's state every step as *observation frames*: 11-bit frames from `0x7F0`, which exist only between the runner and the oracle, never on a bus or in a frame log (`sim/l2/l2_accept.h`). On the board, `open-crank-hil` computes the same observables from the same plant (below).

| Observable | Value | Default |
|---|---|---|
| `plant_rpm` | the plant's crankshaft speed | 0 |
| `ecu_rpm` | the speed in the ECU's EEC1, read with its own clock | gap |
| `misplaced` | injections outside every cylinder's window, or on another cylinder's channel, since the start | 0 |
| `misfires_1s` | windows that closed with no injection, in the last second | 0 |
| `first_injection_rev` | crank revolutions from rest to the first injection that counted | gap |
| `soi_err_deg` | the last counted injection's start, minus the angle the ECU commands (10° before its TDC) | gap |
| `fuel_pct` | the fuel the plant burns, the mean of its six cylinders | 0 |

**The scenarios** (`scenarios/l2/`, each with its `.accept.csv`; ctest `l2_accept_<name>`):

| Scenario | What it shows |
|---|---|
| `start_to_idle` | from rest to the ECU's own low idle, 750 rpm |
| `hold_750`, `hold_1500`, `hold_2100` | a speed asked by the controller, held within ±0.5 % |
| `hoist_step` | Level 1's load step: 570 Nm on and off at 1500 rpm |
| `crank_tooth_lost` | one crank edge never reaches the ECU (`fault.crank_teeth_lost`): it loses sync, stops injecting, resynchronises |
| `overspeed` | an external torque drives the engine past 2400 rpm; the fuel stays cut until it stops |

**The exit criteria of v0.4** (Level 2 design, decision 13), in every scenario's criteria:
- **"Synchronises":** `misplaced == 0` throughout; `first_injection_rev <= 3.5` (FULL sync within three revolutions of rest, plus the way to the next start of injection); `misfires_1s == 0` once running. In `hoist_step` the second after the release is left out of that last one (C3a until the release, C3b from 2 s after it): on the overshoot the governor cuts the fuel to 0 for about 60 ms, the plant cannot tell a cut from a misfire, and `misfires_1s` counts the last second. D2 and R2 judge the release.
- **"Holds a requested speed":** `plant_rpm` within ±0.5 % of the request from 6 s on. The band covers the HSI: the ECU holds what it reads, so the plant runs about 0.27 % low (1496 rpm for 1500).
- **The load step** (`hoist_step`): back within ±0.5 % within 3 s of the step and of its release. The dip may not go below 1207 rpm: the in-memory loop's own dip (244 rpm, to 1255.8 rpm, 0.39 s after the step) plus 20 %. The overshoot on release is held to the loop's 304 rpm plus 20 %, 1865 rpm.

**The jitter budget** (v0.4 exit criterion 1), written by `--jitter` for every injection counted after the first second of running, in the style of open-tick's hard-run budget:

```
# oc-l2-jitter 1; source=open-crank-l2-run; scenario=scenarios/l2/hold_1500.csv; hsi_bias_ppm=-2658; delay_steps=30
quantity,declared,measured,unit,within
injections,,908,count,
soi_err_p50_deg,,0.000225,deg,
soi_err_p99_deg,0.1,0.019792,deg,yes
soi_err_max_deg,0.5,0.028448,deg,yes
width_err_max_us,,1.725505,us,
width_err_max_rel,,0.002745,,
width_err_over_0.3pct_us,1,-0.052619,us,yes
```

In memory the start of injection lands within 0.03° of its angle at every speed (the ECU extrapolates from its last tooth period; the timer ticks and the bridge's ticks add the rest), and every width is 0.27 % long, the HSI. On the board the same file comes from the bridge's captures (below).

### On the board, in a closed loop

`open-crank-hil` (`platforms/linux-socketcan/hil_main.c`) runs the same scenarios and criteria against a real engine ECU: the reference firmware `oc-stm32g4-ecu` on a NUCLEO-G431KB ([platforms/stm32g4/README.md](../platforms/stm32g4/README.md#the-engine-ecu-level-2)), wired to open-tick's Tang Nano 20K bridge ([WIRING-L2.md](../platforms/stm32g4/WIRING-L2.md)), with TSC1 and EEC1 on a CAN interface.

- **The loop.** The machine side is the in-memory loop's own code with an external ECU (`oc_l2_loop_init_external()`): the plant, the pump, the controller and the operator panel, one step per 1 ms, through open-tick's hard loop (H = 16). Each step's crank speed goes to the bridge as its `crank_phase_inc`, so the bridge's crank and cam are the plant's angle; the bridge captures the six injector lines, and each capture fuels the plant by its own tick. The controller's TSC1 and DM1 go out at their step on the bridge's clock; the ECU's EEC1 comes back to the controller and to the oracle.
- **The same criteria** as in memory, on the same observables; the jitter budget comes from the bridge's captures. Only the widths are not judged (the host does not know the width the ECU commanded): they are measured at constant speed by `tools/board/ecucheck.py`.
- **The fidelity check** first (`--fidelity sim/l2/fidelity.csv --bridge-fidelity <open-tick>/gateware/tang_nano_20k/fidelity.csv`): crank up to 2400 Hz with 1 µs of jitter, cam 20 Hz, six captures at 20 a second with at most 40 ms from the DUT to the plant. A bridge that cannot guarantee them is refused before it is opened (exit 5).
- **Outputs:** the trace, the frame log (`bus_clock=bridge`), the verdicts, the jitter budget and open-tick's budget of the hard run. Exit 0 pass, 1 a criterion failed, 2 an error, 3 the hard run was not ok, 5 refused by the fidelity check.

`tools/board/hilrun.py` runs every scenario the board can play (all but `crank_tooth_lost`, a fault of the in-memory edge model): it programs the ECU, loads the bridge fresh and resets the NUCLEO before each scenario, keeps the ECU's USART2 rows next to each run, and retries once a hard run that was not ok.

```sh
eval "$(external/open-tick/tools/fpga_toolchain.sh)"   # openFPGALoader
tools/board/hilrun.py --hil build/open-crank-hil --bitstream <open-tick>/build-gateware/bridge.fs --out hil-out
```

**On the reference bench** (2026-11-28, [measurements/2026-11-28-hil/](measurements/2026-11-28-hil/)): every scenario passed every criterion, 29 of 29. Every injection in its own window; the first one within 2.31–2.64 revolutions; the plant at 747.8–748.2, 1496.1–1496.7 and 2093.8–2096.6 rpm for 750, 1500 and 2100; `hoist_step`'s dip to 1267.9 rpm and overshoot to 1787.5 rpm; the start of injection within 0.0264° at p99 and 0.0301° at most. The captures reached the plant 29–30 ms after they began. Two of the eight hard runs were `degraded` (3 underruns each, every criterion still passed) and were retried: at 1 ms steps with H = 16, a sender that wakes about 1 ms late can cost a few steps.

## Link faults

The four scenarios added in v0.3 inject transport faults with open-tick's links ([open-tick `docs/link-faults.md`](https://github.com/Open-Heavy-Machinery/open-tick/blob/main/docs/link-faults.md)). The simulated machine has two:

| Link | Carries | Applied in | Signals |
|---|---|---|---|
| `mc` | what the DUT sends to the machine: TSC1 and OC_PUMP_CMD | `oc_bench_rx()`, after the oracle has seen the frame | `fault.link.mc.drop`, `.drop_one_in`, `.delay_ms`, `.jitter_ms`, `.corrupt`, `.bus_off` |
| `engine` | what the engine sends to the DUT: EEC1, ET1 and EFL/P1 (not the operator panel's frames) | `oc_bench_step()`, before the frames go on the bus | `fault.link.engine.*`, the same six |

- **No filters**: a fault applies to every frame of its direction. With `drop_one_in = 3` on `mc`, every third frame is lost; since TSC1 and OC_PUMP_CMD alternate, that is every third TSC1 and every third OC_PUMP_CMD.
- **Fixed seeds** for jitter (`OC_BENCH_LINK_MC_SEED` 1, `OC_BENCH_LINK_ENGINE_SEED` 2), so a scenario gives the same trace on every run in memory.
- **The same bench everywhere**: `open-crank-run`, `open-crank-accept-loop`, `open-crank-sil` and `open-crank-accept` all use it, so the fault is the same in memory, on `vcan` and with a board. On `vcan` and with a board, the oracle sees the DUT's frames on the bus, before the `mc` link, and the engine's frames after the `engine` link, as the DUT sees them.
- **A full link breaks the run.** A link holds up to 64 frames in flight; a frame pushed into a full one is lost although no fault asked for it. Both acceptance runners then give no verdict and exit with code 2. 300 ms of delay on the engine's 10 ms EEC1 needs about 32 places.
- With no link fault set, the links are transparent: the traces, reports and verdict files of the seven scenarios that existed before them are the same, byte for byte, except for the `SR-n: ` prefixes in the criteria's text.
- `fault.mc_tx_lost` and `fault.engine_tx_lost` are kept as they were, so the older scenarios do not change. New scenarios use the links.
- `corrupt` is used by `speed_command_corrupt` (v0.3.1): byte 3 of every frame the DUT sends, the high byte of TSC1's requested speed, is XORed with `0xFF` for 6 s. 1500 rpm would read as about 6716 rpm. The engine refuses the frames by their checksum (SPN 4207); the oracle still sees the DUT's frames as sent, before the link. In OC_PUMP_CMD byte 3 is unused.

Reaction times of the reference controller in memory (`open-crank-accept-loop`; the criteria count includes G3 since v0.3.1):

| Scenario | Criteria | Reactions |
|---|---|---|
| `engine_status_delay` | 9 | S2 990 ms, E1 20 ms, D1 210 ms, D2 530 ms |
| `engine_status_jitter` | 7 | S2 990 ms, J2 20 ms |
| `speed_command_intermittent` | 7 | S2 990 ms, I2 20 ms |
| `mc_bus_off` | 8 | S2 990 ms, B1 710 ms, B2 530 ms |
| `speed_command_corrupt` (v0.3.1) | 8 | S2 990 ms, K1 710 ms, K2 530 ms |
| `engine_dtc_red` (v0.3.1) | 9 | S2 990 ms, E1 20 ms, R1 10 ms, R2 530 ms |
| `engine_dtc_amber` (v0.3.1) | 9 | S2 990 ms, E1 20 ms, A1 600 ms, A3 600 ms |
| `engine_dtc_red_and_amber` (v0.3.1) | 6 | S2 990 ms, M1 110 ms |

On the reference board (`tools/board/bench.sh`, 2026-11-25, open-crank `18060be`) all four pass with 0 late steps, and every reaction is within one 10 ms step of the times in memory:

| Scenario | Reactions on the board |
|---|---|
| `engine_status_delay` | S2 1000 ms, E1 20 ms, D1 200 ms, D2 540 ms |
| `engine_status_jitter` | S2 990 ms, J2 30 ms |
| `speed_command_intermittent` | S2 1000 ms, I2 20 ms |
| `mc_bus_off` | S2 1000 ms, B1 710 ms, B2 540 ms |

`speed_command_corrupt` (v0.3.1, step 3b) on the board: S2 1000 ms, K1 720 ms, K2 540 ms, with G3 holding in all twelve scenarios: the board's firmware sends the counter and the checksum.

The engine's DM1 scenarios (v0.3.1, step 3d) on the board: `engine_dtc_red` R1 10 ms, R2 540 ms; `engine_dtc_amber` A1 600 ms, A3 600 ms; `engine_dtc_red_and_amber` M1 110 ms. The board's firmware reassembles the BAM as the simulator does.

These are the board's reference times in `tools/board/bench_reference.csv` (v0.3.1, step 3a), so `bench.sh` now checks every reaction of the eleven scenarios against the reference bench.

## Results

These are the v0.2.1 results, for the seven scenarios of that release. Run on 2026-11-23 against the reference controller, one run per scenario, each from a fresh start:

- on `vcan`, with `open-crank-mc` as the device under test (`tools/accept_vcan.sh`, in CI's `vcan-demo` job);
- on the reference board: a NUCLEO-G431KB running the v0.2 demo unchanged (`portable/` is the same since v0.1.1), through the CANable adapter at 250 kbit/s (see [platforms/stm32g4/README.md](../platforms/stm32g4/README.md)), reset by hand before each scenario.

All seven scenarios pass on both, with 0 late steps. The board's reports and traces are attached to the GitHub release of v0.2.1.

| Scenario | Criteria | `vcan` | Board | Reaction times on the board |
|---|---|---|---|---|
| `cold_start` | 5 | PASS | PASS | S2 990 ms |
| `engine_status_loss` | 6 | PASS | PASS | S2 990 ms, E1 20 ms, E2 210 ms |
| `hoist_step` | 7 | PASS | PASS | S2 1000 ms, H2 20 ms, H3 30 ms |
| `override_ignored` | 4 | PASS | PASS | O1 1500 ms |
| `overspeed` | 5 | PASS | PASS | S2 990 ms, V1 640 ms |
| `speed_command_loss` | 7 | PASS | PASS | S2 990 ms, L1 710 ms, L2 530 ms |
| `target_above_high_idle` | 4 | PASS | PASS | S2 990 ms |

Every reaction time on the board is the same as on `vcan`, except in `hoist_step`, where S2 and H3 come one 10 ms step later (1000 ms against 990 ms, 30 ms against 20 ms): the round trip through the adapter (about 1.7 ms) is small against the 10 ms step, so a reaction lands in the same step or in the next one. The tightest margin is E2's, 210 ms against 300 ms.

What this does not show: one run per scenario on the board, on a two-node bench, not on a vehicle network with other traffic.

## Deliberately broken controllers

Exit criterion 2 of v0.2.1 is that a deliberately broken controller fails. A suite that always passes says nothing: an oracle that sees no frame at all would pass the reference too. So each mutation of the reference controller that `tools/mutation_check.sh` applies is also built and graded by the suite, and it must fail the criteria that belong to it.

`tools/accept_mutants.sh` copies the tree to a scratch directory, grades the unmutated reference on every scenario with `open-crank-accept-loop` (and stops if it already fails), then applies each mutation, rebuilds, and grades the mutant on **all** scenarios (twelve), without stopping at the first failure: the full list shows which criteria have teeth. It prints one line per mutation (`killed`, `accepted`, or a problem: `SURVIVED`, `WRONG`, `UNEXPECTED`, `ERROR`, `STALE`, `NO BUILD`) and exits with 0 only if there is no problem. CI runs it in the `mutation` job.

It runs in memory, not on `vcan`, on purpose: the loop is deterministic, so no timing jitter can kill a mutant for the wrong reason; it takes a few seconds instead of about 13 minutes; and it does not depend on the CI runner providing `vcan`. The path over the bus is covered by the reference's runs on `vcan` and on a board.

All mutations are in `portable/machine_ctrl/src/machine_ctrl.c`:

| Mutation | Must fail |
|---|---|
| controller: engine status timeout ignored | E2 |
| controller: overspeed ignored | V1 |
| controller: never checks that the engine honours the override | O1, L1 |
| controller: pump stays commanded outside RUN | G2, E2 |
| controller: override kept outside RUN | S1, E2, O1, V1, L1 |
| controller: operator target not clamped to the governed range | G1 |
| controller: speed request not slew-limited | nothing: accepted survivor |
| controller: EEC1 timeout multiplied by 5 | D1 (and E2) |
| controller: faults on the first late EEC1 (EEC1 timeout cut to 20 ms) | J1 |
| controller: sends only every 150 ms (no margin for a lost frame) | I2 |
| controller: waits 5 s for the engine to honour the override | B1 (and O1, L1) |
| controller: TSC1 checksum not written | G3 |
| controller: TSC1 counter frozen | G3 (and S2 in every scenario: the engine refuses every repeated counter) |
| controller: the engine's red stop lamp ignored | R1, M1 |
| controller: the engine's amber lamp ignored (no limp-home) | A1 |
| controller: a DM1 that comes as a BAM is ignored | M1 |
| controller: its own DM1 never shows the red stop lamp | F1 |
| controller: its own DM1 sent only every 5 s | F1 |
| controller: its own DM1's SPN does not say which fault | F1 |

Four are one per link-fault scenario (v0.3); the last two check the TSC1 counter and checksum (v0.3.1). The checksum mutant is in `portable/j1939/src/j1939.c`, the codec the controller uses. The last three check the engine's DM1 (v0.3.1, step 3d); the BAM mutant is in `portable/j1939/src/j1939_diag.c`. The last three check the controller's own DM1 (SR-14). In total: 18 mutants killed, 1 accepted survivor.

**Why the mutant of `speed_command_intermittent` is killed by I2 and not by I1.** Losing one frame in three is invisible to a controller that sends every 10 ms: the engine's 200 ms request timeout never expires, so its EEC1 keeps showing the controller in control, and the controller has nothing to react to. No one-line change to the reference makes it fail I1, which is a robustness check: it fails a controller that would react where it should not. The defect the loss exposes is a controller that sends too seldom. The mutant that sends every 150 ms fails I2, because its pump command lags the lever by more than 100 ms. It fails the same check in every scenario with the hoist, with or without frame loss.

**Accepted survivor: speed request not slew-limited.** The reference ramps its requested speed at 500 rpm/s (`MC_DEFAULT_PARAMS`), but nothing in the bus contract asks for a ramp, and the engine's governor accepts a step in its setpoint. A controller that jumps straight to the target is not unsafe, so the suite does not fail it.

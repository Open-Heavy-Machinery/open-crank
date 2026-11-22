# Board vs paced SIL: tolerances for `hoist_step`

Exit criterion 1 of [v0.2](../ROADMAP.md#v02-reference-board-on-the-bus) asks for the latency first, then the tolerances, then the comparison. The latency is measured in [`platforms/stm32g4/README.md`](../platforms/stm32g4/README.md#measuring-the-usb-can-round-trip). This page states the tolerances, written before any comparison, and how to run it. [`tools/compare_runs.py`](../tools/compare_runs.py) checks them.

## What is compared

- **Reference:** the SIL paced by the wall clock with the reference controller on `vcan`: `open-crank-mc vcan0` and `open-crank-sil vcan0 scenarios/hoist_step.csv --trace ref.csv`.
- **Board:** the same scenario with the board in the loop: `open-crank-sil can0 scenarios/hoist_step.csv --trace board_N.csv`.

Only columns that are observable on the bus are used: `t_s`, `rpm`, `controlling_sa` and `pump_cmd_pct`. The `mc_*` columns are empty when the controller runs outside `open-crank-sil`.

## Procedure

1. One reference run. Restart `open-crank-mc` before it: when the SIL stops, the controller latches a fault.
2. Three board runs, each started after a board reset (see the STM32G4 README).
3. In every run, `open-crank-sil` must report `0 late steps` when it stops. That line is not in the trace; write it down.
4. `tools/compare_runs.py ref.csv board_1.csv board_2.csv board_3.csv`. All three board runs must pass.

## Tolerances

Times are in seconds from the start of the scenario; "board − reference" is the difference between the same metric in the two traces. The band is 1500 ± 10 rpm.

| Metric | Definition | Limit |
|---|---|---|
| `takeover_s` | first row with `controlling_sa` = `0x27` | ±0.030 s |
| `reach_band_s` | first row at or after 2.0 s inside the band | ±0.030 s |
| `settle_start_s` | settling time in [2.0, 8.0) s | ±0.030 s |
| `pump_on_s` | first row at or after 8.0 s with `pump_cmd_pct` > 1 | ±0.030 s |
| `droop_t_s` | time of the minimum `rpm` in [8.0, 12.0) s | ±0.030 s |
| `settle_load_s` | settling time in [8.0, 18.0) s | ±0.030 s |
| `pump_off_s` | first row at or after 18.0 s with `pump_cmd_pct` < 1 | ±0.030 s |
| `over_t_s` | time of the maximum `rpm` in [18.0, 21.0) s | ±0.030 s |
| `settle_unload_s` | settling time in [18.0, 25.0) s | ±0.030 s |
| `droop_min_rpm` | minimum `rpm` in [8.0, 12.0) s | ±5 rpm |
| `over_max_rpm` | maximum `rpm` in [18.0, 21.0) s | ±5 rpm |
| `mean_5_8`, `mean_12_18`, `mean_21_25` | mean `rpm` in [5, 8), [12, 18), [21, 25) s | ±1 rpm |
| `maxdev_5_8`, `maxdev_12_18`, `maxdev_21_25` | maximum \|`rpm` − 1500\| in the same windows, board run only | ≤ 2 rpm |
| `control_held` | every row from `takeover_s` to the end has `controlling_sa` = `0x27`, in each run | must hold |

The settling time in a window is the time of the row after the last row of the window that is outside the band; if no row is outside, it is the start of the window.

## Why these limits

- **Latency.** The round trip through the USB-CAN adapter is about 2 ms (p50 1.660 ms, max 4.170 ms). The board and the SIL run on clocks that are not synchronised, so a frame can also wait up to one 10 ms step in each direction. Together that is about 20 ms in the worst case; ±30 ms (three steps) leaves a margin.
- **Delay moves events, not amplitudes.** With extra bus delays of 20 to 30 ms in each direction, or the controller's clock 1 % fast or slow, the free-running loop shifts every event by about the added delay, and the droop minimum, the overshoot maximum and the steady speed do not change. That was a one-off experiment on a scratch copy of the code, not something in this repository.
- **Amplitudes ±5 rpm** are about 3 % of the 170 rpm droop: wide enough for sampling, narrow enough to catch a controller that reacts differently.

## What this comparison does not show

The engine speed loop is closed by the simulated engine ECU. The controller sends the target speed (TSC1) and the pump command (OC_PUMP_CMD). So the comparison checks that the board gives the same commands at the same times as the reference controller. It does not grade the quality of speed regulation.

## Results

None yet *(planned)*.

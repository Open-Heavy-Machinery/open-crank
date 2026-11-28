# Parameters and their provenance

Every number that shapes the simulated behaviour is listed here with where it came from. The plant is **representative of its class, not calibrated against any real engine**. If you have a better source for a value, open an issue and cite it.

Provenance labels:

| Label | Meaning |
|---|---|
| **Inherited** | Came with the first vertical slice; the original source was not recorded. Treat it as an estimate until a source is cited. |
| **Estimate** | Engineering judgement, with the reasoning given. |
| **Derived** | Computed from other entries in this file. |
| **Choice** | A project decision, not a property of any engine. |
| **J1939** | Fixed by the message definition; verify against your copy of SAE J1939-71. |

## Plant `d936la6` (`sim/plants/d936la6/src/d936la6.c`)

| Parameter | Value | Provenance | Notes |
|---|---|---|---|
| `inertia_kgm2` | 1.85 kg·m² | Inherited | Engine, flywheel and pump drive together. Plausible for a 10.5 L six. |
| `full_load_brake` (rpm → Nm) | 600 → 950, 1000 → 1280, 1350 → 1450, 1600 → 1380, 1900 → 1200 | Inherited | Read as **brake** torque: 1200 Nm at 1900 rpm is 239 kW, matching the "~240 kW" class figure in the README. Earlier code treated this table as indicated torque and subtracted friction again, so the engine delivered about 207 kW. |
| `full_load_brake` 2100 rpm | 1090 Nm | Estimate | Constant power (≈240 kW) extended to the governed ceiling. The old 2200 rpm → 300 Nm entry was a governor break-off curve and was removed: the ceiling belongs to the ECU. |
| `full_load_brake` below 600 rpm | clamped to 950 Nm | Choice | The ECU limits fuel at these speeds (start fuel), so the exact shape matters little. |
| `reference_torque_nm` | 1600 Nm | Choice | J1939 reference torque (SPN 544) used for percent-torque values. Chosen just above peak indicated torque (1450 + 126 ≈ 1576 Nm). |
| `friction_c0_nm` | 45 Nm | Inherited | Coulomb part of friction and pumping losses. Also the static breakaway torque at standstill. |
| `friction_c1_nm_per_rpm` | 0.06 Nm/rpm | Inherited | Speed-dependent part. 159 Nm at 1900 rpm (13 % of full-load torque), a plausible share. |
| `boost_floor_frac` | 0.45 | Estimate | Share of full-load torque available with no boost, roughly a naturally aspirated engine of the same size. |
| `boost_tau_s` | 1.2 s | Estimate | Turbocharger lag. From zero boost at full fuel the effective time constant is `boost_tau / boost_floor` ≈ 2.7 s, within the 1–5 s load-acceptance range typical of heavy-duty turbo-diesels. |
| `starter_stall_torque_nm` | 300 Nm | Estimate | Referred to the crankshaft. Chosen with the next entry so that the starter alone cranks the engine at ≈200 rpm (typical heavy-diesel cranking: 100–250 rpm). |
| `starter_no_load_rpm` | 250 rpm | Estimate | See above. Above this speed the starter freewheels. |
| `coolant_initial_c` | 20 °C | Choice | Ambient at start. |
| `coolant_warm_c`, `coolant_gain_c_per_pct` | 75 °C, 0.25 °C/% | Inherited | Placeholder thermal model: settled temperature 75–100 °C with fuel. |
| `coolant_tau_s` | 100 s | Inherited | Placeholder. |
| `oil_min_rpm`, `oil_base_kpa`, `oil_kpa_per_rpm`, `oil_max_kpa` | 50 rpm, 120 kPa, 0.22 kPa/rpm, 550 kPa | Inherited | Placeholder: 285 kPa at idle, 450 kPa at 1500 rpm, relief at 550 kPa. |
| `oil_tau_s` | 0.5 s | Inherited | Placeholder. |

## Simulated engine ECU for `d936la6` (`sim/plants/d936la6/src/d936la6_ecu.c`)

| Parameter | Value | Provenance | Notes |
|---|---|---|---|
| `low_idle_rpm` | 750 rpm | Inherited | Was `idle_rpm` in the plant. |
| `high_idle_rpm` | 2100 rpm | Inherited | Was `max_rpm` in the plant. Highest accepted speed request. |
| `overspeed_trip_rpm` | 2400 rpm | Estimate | About 15 % above the governed ceiling. |
| `fuel_enable_rpm` | 100 rpm | Estimate | Speed by which a real ECU has crank and cam synchronisation. |
| `running_rpm` | 500 rpm | Estimate | Starter released and governor in control above this speed. |
| `stall_rpm` | 300 rpm | Estimate | A running engine below this speed is declared stalled; fuel is cut. |
| `start_fuel_pct` | 30 % | Estimate | Gives run-up from cranking to idle in about 0.5 s with a small overshoot. |
| `max_crank_s` | 15 s | Estimate | Typical starter duty limit. |
| `kp_pct_per_rpm`, `ki_pct_per_rpm_s` | 0.10 %/rpm, 0.30 %/(rpm·s) | Derived | Tuned for ≈5 rad/s closed-loop bandwidth with ζ ≈ 0.9 at 1500 rpm (≈15 Nm per % fuel, J = 1.85 kg·m²). |
| `droop_pct` | 0 % | Choice | Isochronous by default. Droop exists for generator-set load sharing (v0.7). |
| `request_timeout_ms` | 200 ms | Choice | 20 missed TSC1 periods. Verify against the behaviour of the engine you target. |
| `source_address` | 0x00 | J1939 | Preferred address of Engine #1. |

## Level 2 plant for `d936la6` (`sim/plants/d936la6/src/d936la6_l2.c`, `sim/l2/`, v0.4)

The Level 2 plant is the same `d936la6` plant above, stepped every 1 ms, with a crank angle and fuel from captured injector pulses ([ARCHITECTURE.md §4.1](../ARCHITECTURE.md#41-plant-core-simcore-simplants)). These are its own numbers.

| Parameter | Value | Provenance | Notes |
|---|---|---|---|
| `injector_dead_us` | 200 µs | Estimate | A pulse this short delivers no fuel. Of the order of a common-rail solenoid injector's opening delay. |
| `injector_full_us` | 1600 µs | Estimate | Width above the dead time for 100 % fuel, so full load is a 1.8 ms pulse: about 16° at 1500 rpm and 26° at 2400 rpm, plausible for a heavy-duty diesel at full load. The model is linear: fuel_pct = 100 × (width − dead) / full. |
| `window_before_deg`, `window_after_deg` | 40°, 20° | Choice | An injection counts for a cylinder when it begins between 40° before and 20° after that cylinder's compression TDC. Wide enough for any sensible start of injection, narrow enough that an injection on the wrong cylinder, or 360° off, never counts. The windows of two cylinders never overlap (one TDC every 120°). |
| `capture_delay_steps` | 20 steps (20 ms) | Choice | How long a window waits for its injection before it is judged a misfire. Above a hard run's DUT-to-plant delay with H = 16 (17.3 ms, open-tick `docs/bridge-protocol.md` §2). |
| step | 1 ms | Choice | Level 2's step (`ROADMAP.md` v0.4): 27 000 ticks of the bridge's 27 MHz clock. |
| wheel | 60−2; the first tooth after the gap begins 60° before TDC 1 | Choice | TDC 1 is the start of tooth 10; the gap lies 48°–60° after the previous TDC, away from every window. |
| cam | high from 90° to 120° of the 720° cycle | Choice | One pulse per cycle, in its first revolution, away from the gap. |
| firing order | 1-5-3-6-2-4 | Choice | The usual inline-six order: TDCs at 0°, 120°, 240°, 360°, 480°, 600° for cylinders 1, 5, 3, 6, 2, 4. |

## Hydraulic hoist pump (`sim/loads/src/hydraulic_pump.c`)

| Parameter | Value | Provenance | Notes |
|---|---|---|---|
| `displacement_max_cc_rev` | 140 cm³/rev | Inherited | Mobile-crane-sized open-circuit pump. |
| `gear_ratio` | 1.0 | Choice | Pump on the crankshaft speed. |
| `hm_efficiency` | 0.90 | Inherited | |
| `standby_pressure_bar` | 20 bar | Inherited | |
| `ls_margin_bar` | 20 bar | Estimate | Load-sensing margin, typically 15–30 bar. |
| `relief_pressure_bar` | 300 bar | Inherited | Was `max_pressure_bar`. |
| `standby_displacement_frac` | 0.03 | Estimate | A destroked pump still covers leakage. The old model held full displacement at standby (≈50 Nm at idle); this gives ≈1.5 Nm. |
| `active_threshold_pct` | 1 % | Choice | Below this command no function is active. |
| `displacement_tau_s` | 0.10 s | Estimate | Swash-plate response. |
| `pressure_tau_s` | 0.25 s | Inherited | Was the hard-coded pressure ramp. |

## Reference machine controller (`portable/machine_ctrl/src/machine_ctrl.c`, `MC_DEFAULT_PARAMS`)

| Parameter | Value | Provenance | Notes |
|---|---|---|---|
| `source_address` | 0x27 | Choice | A supervisory-controller address from the J1939 preferred-address table. Not 0x03, which is the preferred address of Transmission #1. Verify against J1939 (Table B2) and change it if your network uses 0x27. |
| `engine_address` | 0x00 | J1939 | Engine #1. |
| `eec1_timeout_ms` | 200 ms | Choice | 20 missed EEC1 periods. |
| `operator_timeout_ms` | 200 ms | Choice | 10 missed operator messages; then target falls to idle and the hoist command to zero. |
| `run_confirm_ms` | 500 ms | Choice | Engine speed must stay above `running_rpm` this long before RUN. |
| `override_confirm_ms` | 500 ms | Choice | Time allowed for EEC1 SPN 1483 to show this controller in control. |
| `limp_rpm` | 1200 rpm | Choice (David, 2026-11-25) | Limp-home on the engine's amber lamp: requested speed at most this. Reached at the slew limit. |
| `limp_pump_pct` | 50 % | Choice (David, 2026-11-25) | Limp-home: pump command at most this. |
| `limp_pump_rate_pct_per_s` | 100 %/s | Choice | How fast the pump limit moves into and out of limp-home: 85 % to 50 % in 0.35 s, no step. |
| watchdog timeout (`OC_WDG_TIMEOUT_MS`, reference board) | 100 ms | Choice | 10 control periods. Below the engine's 200 ms request timeout, so a hung controller is restarted before its last TSC1 would expire on its own. LSI 32 kHz / 32, reload 99 (`platforms/stm32g4/board/watchdog.c`). |
| `running_rpm` | 600 rpm | Choice | Between the ECU's running threshold (500) and low idle (750). |
| `stall_rpm` | 400 rpm | Choice | Above the ECU's own stall threshold (300), so the controller notices first. |
| `overspeed_rpm` | 2300 rpm | Choice | Above the governed ceiling (2100), below the ECU trip (2400). |
| `request_min_rpm`, `request_max_rpm` | 750, 2100 rpm | Derived | The ECU's governed range. |
| `slew_rpm_per_s` | 500 rpm/s | Choice | Idle to 1500 rpm in 1.5 s. |
| control period | 10 ms | Choice | Platform loop period; TSC1 and OC_PUMP_CMD are sent every period. 10 ms is the TSC1 rate commonly given for engines; verify against J1939-71. |

## Bench (`sim/runtime/src/bench.c`, `OC_BENCH_D936LA6`)

| Parameter | Value | Provenance | Notes |
|---|---|---|---|
| `step_ms` | 10 ms | Choice | Physics step (explicit Euler). Matches the fastest message period. |
| `eec1_period_ms` | 10 ms | J1939 | EEC1 rate given for engine speed. |
| `eflp1_period_ms` | 500 ms | J1939 | |
| `et1_period_ms` | 1000 ms | J1939 | |
| `operator_period_ms` | 20 ms | Choice | Typical CAN joystick rate. |
| `dm1_period_ms` | 1000 ms | J1939 | DM1 once a second, and at once when the active codes change (J1939-73). With two codes it is a BAM, packets 50 ms apart (J1939-21: 50 to 200 ms). |
| `pump_cmd_timeout_ms` | 200 ms | Choice | A stale pump command counts as zero, as a de-energised proportional solenoid would. |
| bench source address | 0xFC | Choice | Operator panel and environment. Chosen from the top of the address range, away from common preferred addresses; verify it is free on your network. |

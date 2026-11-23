# Changelog

All notable changes to this project. The project is pre-alpha; nothing here is a stable API yet.

## Unreleased

### Added

- Acceptance suite over the bus (v0.2.1, first part), described in `docs/acceptance.md`: `open-crank-accept` plays a scenario with the simulated machine on SocketCAN and grades the device under test only by the frames it sends; a generic oracle (`sim/oracle/`, only the C standard library and open-tick's core) reads criteria written as data; criteria for the six scenarios (`scenarios/*.accept.csv`); and `tools/accept_vcan.sh`, which runs the suite against the reference controller on `vcan`, in CI's `vcan-demo` job when the runner provides `vcan`.

### Changed

- `README.md` and `ARCHITECTURE.md` no longer list as planned what v0.1.x and v0.2 delivered.

## v0.2: Reference board on the bus (2026-11-22)

The reference controller, unchanged, runs on an STM32G431 (NUCLEO-G431KB) and controls the simulated engine over a real CAN bus at 250 kbit/s. All three exit criteria of v0.2 are met: `hoist_step` with the board in the loop stays within tolerances written before the comparison (three runs, after measuring the USB-CAN round trip); the bring-up guide was followed from scratch on a clean clone; and the worst observed execution time of `mc_step` (DWT) and its worst-case stack (call graph, in CI) are documented. Nothing in `portable/`, `sim/` or `scenarios/` changed since v0.1.1.

### Known limitations

- The board runs on its internal HSI oscillator, not a crystal: enough for a two-node bench, not for a vehicle network.
- The demo does not recover from bus-off.
- The `late steps` line of `open-crank-sil` was not recorded for the comparison runs.
- The STM32 firmware is built in STM32CubeIDE; no CI job builds it.

### Added

- STM32G4 demo of the machine controller on a real board (v0.2 step 1, tested on the board, 2026-11-21): `platforms/stm32g4/`.
- `tools/check_misra.sh` also fails when a baseline pair has no row in the table of `docs/misra_deviations.md`, when the row's count differs from the baseline's, or when its deviation is not approved. Exit criterion 2 of v0.1.x is now checked by CI, not only by review.
- `docs/bus-interface.md` states the bus: classical CAN, 29-bit, 250 kbit/s (J1939-11), J1939-21 framing. The STM32G4 demo samples at 87.5 % and says to reset the board between runs.
- `tools/can_rtt.py`: round-trip time of a CAN echo (request `0x18FF2EFC`, echo `0x18FF2F27`), to measure the USB-CAN adapter's latency for v0.2; standard-library python3 only.
- `platforms/stm32g4/echo/`: echo firmware's `USER CODE` zones for `tools/can_rtt.py`, and the first round-trip figure on the board (2026-11-22: p50 1.660 ms, p99 1.949 ms, max 4.170 ms, 0 of 5000 lost).
- `docs/board-vs-sil.md` and `tools/compare_runs.py`: the tolerances for `hoist_step` with the board in the loop against the paced SIL, written before any comparison (v0.2 exit 1), and the script that checks them.
- Results of `hoist_step` with the board in the loop against the paced SIL (2026-11-22, three runs, all within the tolerances of `docs/board-vs-sil.md`).
- Worst observed execution time of `mc_step` on the STM32G431 with the DWT cycle counter, over the six scenarios: 2287 cycles (13.5 µs at 170 MHz, `-O0`); the demo keeps it in `mc_step_cycles_max`.
- Stack budget by call graph in CI (`tools/stack_budget.py`, limit 1024 bytes): worst-case stack of `mc_step` 68 bytes and of `mc_handle_frame` 96 bytes on the Cortex-M4F MinSizeRel build with the CI's GCC 14.2.
- Bill of materials and wiring of the STM32G4 reference bench in `platforms/stm32g4/README.md`.
- `docs/porting.md`: what a platform does to run the controller, the rules it follows, and how to check a port.
- `platforms/stm32g4/BRINGUP.md`: bring-up guide for the STM32G4 reference bench, from a clean clone to `hoist_step` on the board (v0.2 exit criterion 2), followed from scratch by the maintainer on 2026-11-22 with four fixes.

## v0.1.1: open-tick core, MISRA-oriented checks and stack usage (2026-11-20)

The machine-independent parts of open-crank moved to [open-tick](https://github.com/Open-Heavy-Machinery/open-tick), the family's new core, which open-crank now consumes as a git submodule (`external/open-tick`, pinned at its first release, `v0.1.0`). **No behaviour changed**: the trace of every scenario is byte-identical to v0.1's, and CI now checks that on every pull request.

### Changed

- Scenarios are parsed by open-tick against `OC_SIGNAL_TABLE`. Event times are nanoseconds (`t`, `end` instead of `t_ms`, `end_ms`). The error for decreasing times reads "time 1 s is earlier" instead of "time 1.000 s".
- The bench plays scenarios through open-tick's player; the in-memory loop uses open-tick's CAN queues (32 frames each, as before).
- The trace is written through open-tick's trace writer from a column table: `oc_trace_open(OtkTrace *, FILE *)` and `oc_trace_row(OtkTrace *, ...)` replace `oc_trace_header(FILE *)` and `oc_trace_row(FILE *, ...)`.
- `open-crank-sil` and `open-crank-mc` use open-tick's pacer and SocketCAN bridge. `open-crank-mc` no longer links the simulator.
- The mutation "scenario: unknown signal names accepted" moved to open-tick with the parser. It is replaced by "scenario: events never reach the bench's signals"; there are still 18.

### Added

- CI runs once per commit: on `push` to `main`, and on `pull_request` for branches. Before, every pull request ran twice.
- `sim/can_otk`: conversion between `J1939Frame` and open-tick's CAN frames, as a library of its own.
- `tools/compare_traces.sh` and the CI `traces` job: byte-for-byte trace comparison against each pull request's base branch.
- `tools/vcan_demo_check.sh` and the CI `vcan-demo` job: the README's `vcan` demo, run and checked automatically.
- `tools/check_misra.sh` and the CI `misra` job (on `ubuntu-26.04`): cppcheck's MISRA addon over `portable/`, with the target's type sizes (`--platform=arm32-wchar_t4`), compared with a baseline of today's findings (`tools/misra-baseline.txt`, counts per rule and file). It fails on a new finding or a higher count. A MISRA-oriented check: it does not imply compliance. `tools/check_misra_docker.sh` runs it locally in the CI's environment.
- `docs/misra_deviations.md`: the four deviations (DEV-001 to DEV-004) that cover every finding left in the MISRA baseline, each with its scope, reason and mitigation.
- `tools/check_portable.sh` builds MinSizeRel with `-fstack-usage`, prints the stack usage of every function in `portable/`, and fails if any is not `static`.

### Fixed

- 21 of the 49 MISRA-oriented findings in `portable/`, fixed in the code with no change in behaviour (six byte-identical traces, 18 mutations still caught):
  - rule 12.1: explicit precedence (5);
  - rule 15.5: a single exit in `mc_state_name`, `mc_fault_name`, `j1939_from_u8`, `j1939_from_u16` and both `clampf` (14);
  - rule 2.5: the unused private macros `J1939_U8_NA` and `J1939_U16_NA` removed (2).

  The baseline went from 49 to 28 findings. The mutation "codec: 'not available' decoded as a value" follows the new form of `j1939_from_u16`.

### Removed

- `sim/runtime/src/scenario.c`'s parser, `platforms/linux-socketcan/pacer.*` and `socketcan.*`: now in open-tick.

### Documentation

- The organisation's mission is now "Open tools to practise and develop embedded control for machines you cannot keep at home, starting with heavy machinery", the same in every repository, the site and the organisation profile.
- v0.4's signal generator is decided: the Tang Nano 20K, through open-tick's bridge (D11).

## Roadmap revision after v0.1

With v0.1 done, [ROADMAP.md](ROADMAP.md) was reordered. Documentation only; no code changed.

| Change | Why |
|---|---|
| The real board comes before the emulator: v0.2 is a reference board on the bus (it was v0.3) | Connecting your own board to a simulated engine is the premise of the project |
| Renode becomes Track R, a parallel track after v0.2 (it was v0.2) | Renode's support for the STM32G4 is unconfirmed, and the board does not need an emulator |
| New v0.2.1: an acceptance suite of the project's own, over the bus | To grade anyone's firmware by its CAN frames alone, whatever its board, RTOS or language |
| Critical-firmware practice is spread over v0.1.x, v0.2 and v0.3 | MISRA-oriented checks and stack usage in CI (v0.1.x); worst observed execution time (measured with the DWT cycle counter; not a proven bound) and stack measured on the board (v0.2); numbered safety requirements traced to tests, and a safe-state specification (v0.3). MISRA tooling leaves the backlog |
| Two engines on one machine in v0.5, before the marine engine (marine moves to v0.6, PMS to v0.7) | Two instances of the same plant generalise the bench, not the plant; the plant interface is still extracted with the marine engine. The PMS then builds on several generator sets on one bench |
| New mission statements (the organisation's was widened later; see above) | They state what the organisation and open-crank are for. Open Heavy Machinery: "Open tools to practise and develop embedded control for heavy machinery, without access to the machine." open-crank: "A heavy-duty diesel that answers back: connect your own firmware over J1939 and practise against an engine you could never keep at home." |

## v0.1: Level 1 vertical slice (2026-11-18)

### Added

- **Simulated engine ECU** (`sim/engine_ecu`): starter control, fuel enable above synchronisation speed, low-idle and PI speed governor with anti-windup and optional droop, speed requests clamped to the governed range, stale-request timeout, overspeed trip latched until standstill, stall detection, reporting of the controlling address.
- **Portable J1939 codec** for TSC1, EEC1, ET1 and EFL/P1 with saturating encoders, "not available" handling, and identifiers parsed by PGN and destination.
- **Bench messages** `OC_OPERATOR` and `OC_PUMP_CMD` (Proprietary B, documented as non-J1939) so a device under test needs nothing but the bus.
- **Portable machine-controller core** (`portable/machine_ctrl`): `WAIT_ENGINE / RUN / FAULT`, slew-limited speed request, engine-status timeout, overspeed and stall faults, confirmation that the engine honours the override (EEC1 SPN 1483), fault reset.
- **Bench** (`sim/runtime/bench`): the simulated machine as seen from the bus, owning simulated time; **scenarios** as CSV with strict parsing; **in-memory loop**; **CSV trace**.
- `open-crank-run`: headless, free-running closed loop; no CAN bus needed.
- Six scenarios in `scenarios/`, shared by the tests, the runner and the SIL.
- Tests for every module and closed-loop tests for the v0.1 exit scenarios, using a `CHECK` macro that survives `-DNDEBUG`.
- CI: gcc and clang in Debug and Release with warnings as errors, sanitizers, README commands verbatim, Cortex-M4F portability check, mutation checks.
- `docs/parameters.md` (provenance of every parameter), `docs/bus-interface.md` (Level 1 contract).

### Changed

- Repository reorganised into `portable/`, `sim/` and `platforms/` with a one-way dependency rule; `portable/` builds on its own and for Cortex-M4F.
- **Plant is physics only.** Full-load table is brake torque; indicated torque is brake plus friction. Air-path (turbo) state added. Starter modelled as a torque line and engaged from outside. No combustion on a stationary shaft; static friction at standstill. Hard-coded constants moved into parameters; speculative function-pointer hooks and crank angle removed.
- **TSC1 used with its standard meaning**: override mode in byte 0, requested speed in bytes 1-2 at 0.125 rpm/bit.
- Hydraulic load is a load-sensing pump: the controller commands displacement, the load sets pressure; it destrokes at standby.
- The machine controller runs at 10 ms and uses source address `0x27` (it used `0x03`, the preferred address of Transmission #1).
- SIL executables are thin glue over the bench and the controller core; the hoist profile moved from `main.c` to a scenario file.

### Fixed

- The plant started itself from 0 rpm with no fuel and no starter (idle governor hidden in the torque function).
- Friction was subtracted twice from the full-load table: about 207 kW delivered instead of the class's ~240 kW.
- The controller put a throttle percentage in the TSC1 requested-speed bytes while declaring speed control; a real engine would have read about 8000 rpm, in the reserved range.
- The EEC1 encoder converted speed without saturation (undefined behaviour out of range, and no reserved-range protection).
- Tests used `assert()`, which Release builds compile out.

## Roadmap restructuring (before v0.1)

The organisation site and the earlier README listed: v0.1 vertical slice (including Renode, six PGNs, a dashboard), v0.2 aftertreatment, v0.3 fault injection, v0.4 HIL, v0.5 second plant plus PMS. The current [ROADMAP.md](ROADMAP.md) differs as follows.

| Change | Why |
|---|---|
| v0.1 is Level 1, SIL only; Renode moves to v0.2 | A first milestone that includes a plant, a codec, a controller, an emulator and a dashboard is not finishable. The Renode CAN question is also an open risk that should not block the first usable release |
| "Six PGNs" becomes four named PGNs | A number chosen in advance is not a requirement; messages are added when a scenario needs them |
| Dashboard moves to the backlog; a CSV trace and plotting script come first | A viewer is only worth building once there is something worth looking at beyond a console log |
| Aftertreatment moves to the backlog | Large modelling effort that does not exercise the project's core skills (control loops, protocol, timing). Revisit if users ask for it |
| Fault injection moves up to v0.3 and is paired with real hardware | Fault handling is most instructive on real silicon, and the v0.1 slice already covers the basic timeouts |
| v0.5 split into marine engine (v0.5) and electrical plant with PMS (v0.6) | The marine engine tests the plant abstraction; a PMS needs a different physical model. Doing both at once is two milestones |
| Explicit Level 1 / Level 2 | Fixes who is under test and what each message means; makes the FPGA milestone coherent |

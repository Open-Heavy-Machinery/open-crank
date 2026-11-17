# Changelog

All notable changes to this project. The project is pre-alpha; nothing here is a stable API yet.

## Unreleased: v0.1 vertical slice

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

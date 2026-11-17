# Roadmap

Scope is deliberately narrow. Each milestone is small enough to finish and useful on its own. **There are no dates**: a milestone is done when its exit criteria are met, not when a calendar says so. Architecture context is in [ARCHITECTURE.md](ARCHITECTURE.md); what changed and when is in [CHANGELOG.md](CHANGELOG.md).

## At a glance

| Version | Theme | DUT level | Runs on | Status |
|---|---|---|---|---|
| **v0.1** | Closed-loop vertical slice, in CI | Level 1 | laptop (`vcan`, headless) | implemented; two exit checks pending (see below) |
| **v0.2** | Firmware in the loop | Level 1 | emulator (Renode) | not started |
| **v0.3** | Real board on the bus, fault injection | Level 1 | MCU board + USB-CAN | not started |
| **v0.4** | Hard real time | Level 2 | FPGA / MCU I/O backend | not started |
| **v0.5** | Second plant, extract the plant interface | Level 1 (marine) | laptop | not started |
| **v0.6** | Electrical plant and a PMS scenario | Level 1 | laptop, then bench | not started |
| Backlog | Aftertreatment, FMI, dashboard, MISRA tooling, Python bindings | | | |

## Definition of done (every release)

- CI is green on a clean machine.
- The README's build and run instructions work verbatim.
- `docs/parameters.md` lists the provenance of every parameter that changed.
- No claim in the docs is stronger than what the code and tests support.

---

## v0.1: Level 1 vertical slice

**Goal.** One machine controller, talking J1939 on a virtual bus, regulates the speed of one simulated engine under load, and a test proves it.

**In scope**

- Plant `d936la6`: mean-value model with crankshaft speed and air-path (turbo) states, full-load brake-torque table, friction, starter torque line.
- Hydraulic hoist pump load (load sensing: the controller commands displacement, the load sets pressure).
- Simulated engine ECU: starter control, fuel enable, idle governor, TSC1 speed governor, governed ceiling, droop parameter, overspeed trip, TSC1 timeout, stall detection.
- J1939 codec: TSC1 (in); EEC1, ET1, EFL/P1 (out). Two bench messages (operator controls, pump command), documented as non-J1939.
- Portable machine-controller core with `WAIT_ENGINE / RUN / FAULT` states.
- Linux SocketCAN platform and a `vcan` demo; a headless runner that needs no CAN bus.
- Bench-owned simulated time with free-running and wall-clock-paced drivers; scenarios as CSV.
- Headless closed-loop tests; console and CSV trace output.
- CI: host builds, sanitizers, the portability job for Cortex-M4F, mutation checks.
- `docs/parameters.md`, `docs/bus-interface.md`.

**Out of scope.** Renode, FPGA, YAML profiles, dashboard, aftertreatment, thermal fidelity, TSC1 counter and checksum, DM1.

**Exit criteria**

- [ ] 1. **Build and tests pass in CI on a clean machine.** The workflow is in [`.github/workflows/ci.yml`](.github/workflows/ci.yml). Locally the full matrix passes (gcc 11 and clang 14, Debug and Release, warnings as errors, sanitizers). *Pending: the first run on GitHub.*
- [x] 2. **Closed-loop scenarios pass**: cold start to a target speed, load step and release, loss of the speed command, loss of engine status, overspeed protection, plus an engine that ignores the override. [`tests/test_closed_loop.c`](tests/test_closed_loop.c) runs the files in [`scenarios/`](scenarios/).
- [x] 3. **`portable/` compiles for Cortex-M4F with `-Wconversion -Wdouble-promotion` as errors and needs no symbol from outside except the four that GCC requires of any freestanding environment** (`memcpy`, `memmove`, `memset`, `memcmp`). [`tools/check_portable.sh`](tools/check_portable.sh). *Wording changed: "no undefined symbols at all" would fail on legitimate struct copies (ARCHITECTURE.md D16).*
- [x] 4. **At least one deliberate "break the code" check per safety-relevant behaviour confirms that the tests can actually fail.** [`tools/mutation_check.sh`](tools/mutation_check.sh): 18 mutations, all caught.
- [x] 5. **Every plant and ECU parameter has a provenance entry**, as do the controller's and the bench's: [`docs/parameters.md`](docs/parameters.md). Many are labelled *Inherited* (source not recorded) or *Estimate*; that is honest, not final.
- [ ] 6. **The `vcan` demo in the README works as written.** The executables build and the bench they run is the one the tests cover, but the demo needs the `vcan` kernel module and has not yet been run end to end. *Pending: run it on a machine with `vcan` and record the result here.*

**Migration order (one commit each, each verifiable)**

As planned, with the adjustments noted.

1. Move files to `portable/`, `sim/`, `platforms/` with no logic change; then remove the leftover copy of the Linux controller from `portable/`. *Why first:* a pure move is easy to review and keeps `git blame` meaningful.
2. Separate plant physics from the simulated engine ECU. *Why:* biggest conceptual fix; everything after assumes it. *Note:* in this commit the SIL ignored TSC1 (the engine just idled); it was wired again in step 3.
3. TSC1 with its standard meaning, the portable codec, the controller core and the bench. *Why:* follows from step 2; the controller now asks for speed instead of commanding fuel.
4. Closed-loop tests on an in-memory bus. *Why:* they pin the behaviour down before the next refactors can break it. *Note:* the scenario parser arrived here, with scenarios embedded in the test.
5. Scenario files, drivers (free-run, paced) and trace. *Why:* needed by v0.2 and by CI speed.
6. CI with the portability and mutation checks. *Why last:* it enforces what steps 1 to 5 established.

---

## v0.2: Firmware in the loop (Renode)

**Goal.** The same controller source, compiled for a microcontroller and run in Renode, regulates the same simulated engine.

**In scope.** `platforms/renode`, a virtual-time driver that steps the bench in lockstep with the emulator, a CAN link between the emulated controller and the bench, the shared scenario files, first golden-trace comparison against the host build, a minimal trace viewer (a plotting script for the CSV trace that already exists).

**Exit criteria**

1. One named scenario (`scenarios/hoist_step.csv`: cold start to 1500 rpm, then a hoist step) produces speed traces that agree between the host build and Renode within a documented tolerance.
2. It runs headless in CI.

**Risk to retire first.** Confirm that Renode models the CAN peripheral of the chosen target, *before buying a board*. Evidence found so far is for other STM32 families (such as the STM32H7), not confirmed for the STM32G4's FDCAN. If the G4 is not supported, either emulate a different CAN-capable MCU for this milestone or fall back to the host-compiled controller, and keep the real G4 for v0.3. The portable zone makes this a configuration change, not a rewrite.

---

## v0.3: Real board on the bus, and fault injection

**Goal.** A physical STM32 board, running the portable controller, regulates the simulated engine over a real CAN link, and failures are testable.

**In scope.** `platforms/stm32g4` (FDCAN, transceiver wiring notes, startup, reusing `cmake/arm-none-eabi.cmake`), a USB-CAN adapter bridged to SocketCAN, soft-real-time HIL, **fault injection** (CAN dropout, message period jitter, corrupted frames, sensor plausibility faults reported by the engine, limp-home behaviours), and end-to-end latency measurement.

Fault injection pulls in protocol work that v0.1 left out on purpose:

- TSC1 message counter and checksum (SPN 4206/4207), so that corrupted and replayed frames can be rejected.
- DM1 (active diagnostic trouble codes) for faults reported by the engine; with more than one active code, DM1 needs the J1939-21 transport protocol (BAM) on the sending side.

**Exit criteria**

1. The v0.1/v0.2 scenario runs with the real board in the loop.
2. Each injected fault has a test that asserts the controller's reaction.
3. Latency between a plant event and the board's response is measured, with the method and its limits documented.

**Safety note.** This milestone puts real silicon in the loop, never real machinery. The project does not support connecting any of it to operating equipment.

---

## v0.4: Level 2 and hard real time (FPGA)

**Goal.** Your firmware *is* the engine ECU: it reads crank and cam signals and sensors, and commands fuel.

**In scope.** FPGA (or MCU-timer) time and I/O backend, 60-2 crank wheel and cam signal generation (the plant gains crank angle over a full 720° cycle here, not before), analog and digital sensor and actuator I/O, jitter measurement, a reference engine-ECU firmware that achieves crank synchronisation. The plant stays on a CPU or SoC core; the FPGA provides timing and I/O.

**Exit criteria**

1. A jitter budget is stated and measured.
2. The reference ECU achieves sync on the generated waveforms and holds a speed estimate that agrees with the plant's state.

---

## v0.5: Second plant, and extracting the plant interface

**Goal.** Prove the architecture generalises by adding a radically different engine, then extract the plant interface from what both plants actually share.

**In scope.** `9g95mec`: 2-stroke marine engine with a propeller-law load and an ME-style electronic control interface of its own (not J1939, which is not how that engine is controlled). The plant plug-in interface is extracted here, not before.

**Exit criteria**

1. Both plants run on the same runtime and the same scenario runner.
2. A written list of what the second plant forced to change in the core, and why.

---

## v0.6: Electrical plant and a PMS scenario

**Goal.** Make power-management development possible.

**In scope.** A generator-set and electrical-bus model (load sharing, breaker states), and one PMS scenario such as a blackout and recovery sequence. The engine ECU's droop setting, present since v0.1, is what load sharing between sets builds on.

**Why separate from v0.5.** A PMS needs an electrical plant, which is a different physical model from an engine. Splitting it keeps each milestone finishable.

---

## Backlog (not scheduled)

- Power limiting / anti-stall in the reference controller (the bus interface already lets the controller act on its load).
- Speed-dependent turbocharger model; full engine-torque-mode reporting (SPN 899).
- Aftertreatment (DOC, DPF, SCR) with soot and DEF dosing.
- Detailed thermal and air-path models.
- FMI/FMU export for co-simulation.
- Web dashboard (a trace viewer comes first).
- MISRA checking in CI, once a suitable analyser and rule-text setup is chosen.
- Python bindings for scripting scenarios.
- YAML plant profiles, loaded in `sim/` only.

## Public text to update

- The organisation site's "Honest status" paragraph still says the repository holds only a README and a license, and its roadmap list is the previously published one (Renode and a dashboard in v0.1, aftertreatment in v0.2). Update both once v0.1 is merged, following the table at the top of this file.
- An earlier README said the plant "exposes crank and cam position, sensor and actuator interfaces". That describes v0.4; the current README marks Level 2 interfaces as planned. Any copy of the old text elsewhere should be corrected.

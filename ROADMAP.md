# Roadmap

Scope is deliberately narrow. Each milestone is small enough to finish and useful on its own. **There are no dates**: a milestone is done when its exit criteria are met, not when a calendar says so. Architecture context is in [ARCHITECTURE.md](ARCHITECTURE.md).

## At a glance

| Version | Theme | DUT level | Runs on |
|---|---|---|---|
| **v0.1** | Closed-loop vertical slice, in CI | Level 1 | laptop (`vcan`, headless) |
| **v0.2** | Firmware in the loop | Level 1 | emulator (Renode) |
| **v0.3** | Real board on the bus, fault injection | Level 1 | MCU board + USB-CAN |
| **v0.4** | Hard real time | Level 2 | FPGA / MCU I/O backend |
| **v0.5** | Second plant, extract the plant interface | Level 1 (marine) | laptop |
| **v0.6** | Electrical plant and a PMS scenario | Level 1 | laptop, then bench |
| Backlog | Aftertreatment, FMI, dashboard, MISRA tooling, Python bindings | | |

## Definition of done (every release)

- CI is green on a clean machine.
- The README's build and run instructions work verbatim.
- `docs/parameters.md` lists the provenance of every parameter that changed.
- No claim in the docs is stronger than what the code and tests support.

---

## v0.1: Level 1 vertical slice

**Goal.** One machine controller, talking J1939 on a virtual bus, regulates the speed of one simulated engine under load, and a test proves it.

**In scope**

- Plant `d936la6`: mean-value model, one speed state, full-load torque table, friction.
- Hydraulic pump load.
- Simulated engine ECU: idle governor, TSC1 speed governor, governed ceiling, overspeed trip, TSC1 timeout.
- J1939 codec: TSC1 (in); EEC1, ET1, EFL/P1 (out).
- Portable machine-controller core with `WAIT_ENGINE / RUN / FAULT` states.
- Linux SocketCAN platform and a `vcan` demo.
- Clock abstraction with free-run and paced modes; scenarios as CSV.
- Headless closed-loop tests; console and CSV trace output.
- CI, including the portability job for Cortex-M4F.
- `docs/parameters.md`.

**Out of scope.** Renode, FPGA, YAML profiles, dashboard, aftertreatment, thermal fidelity.

**Exit criteria**

1. Build and tests pass in CI on a clean machine.
2. Closed-loop scenarios pass: cold start to a target speed, load step and release, loss of the speed command, loss of engine status, overspeed protection.
3. `portable/` compiles for Cortex-M4F with `-Wconversion -Wdouble-promotion` and has **no undefined symbols**.
4. At least one deliberate "break the code" check per safety-relevant behaviour confirms that the tests can actually fail.
5. Every plant and ECU parameter has a provenance entry.
6. The `vcan` demo in the README works as written.

**Migration order (one commit each, each verifiable)**

1. Move files to `portable/`, `sim/`, `platforms/` with no logic change. *Why first:* a pure move is easy to review and keeps `git blame` meaningful.
2. Separate plant physics from the simulated engine ECU. *Why:* biggest conceptual fix; everything after assumes it.
3. TSC1 with its standard meaning. *Why:* follows from step 2; the controller now asks for speed instead of commanding fuel.
4. Closed-loop tests. *Why:* they pin the behaviour down before the next refactors can break it.
5. Clock abstraction and CSV scenarios. *Why:* needed by v0.2 and by CI speed.
6. CI with the portability checks. *Why last:* it enforces what steps 1 to 5 established.

---

## v0.2: Firmware in the loop (Renode)

**Goal.** The same controller source, compiled for a microcontroller and run in Renode, regulates the same simulated engine.

**In scope.** `platforms/renode`, virtual-time clock mode, a CAN link between the emulated controller and the simulator, a shared scenario runner, first golden-trace comparison against the host build, a minimal trace viewer (CSV plus a plotting script).

**Exit criteria**

1. One named scenario (cold start to 1500 rpm, then a hoist step) produces speed traces that agree between the host build and Renode within a documented tolerance.
2. It runs headless in CI.

**Risk to retire first.** Confirm that Renode models the CAN peripheral of the chosen target. Evidence found so far is for other STM32 families (such as the STM32H7), not confirmed for the STM32G4's FDCAN. If the G4 is not supported, either emulate a different CAN-capable MCU for this milestone or fall back to the host-compiled controller, and keep the real G4 for v0.3. The portable zone makes this a configuration change, not a rewrite.

---

## v0.3: Real board on the bus, and fault injection

**Goal.** A physical STM32 board, running the portable controller, regulates the simulated engine over a real CAN link, and failures are testable.

**In scope.** `platforms/stm32g4` (FDCAN, transceiver wiring notes, startup), a USB-CAN adapter bridged to SocketCAN, soft-real-time HIL, **fault injection** (CAN dropout, message period jitter, corrupted frames, sensor plausibility faults reported by the engine, limp-home behaviours), and end-to-end latency measurement.

**Exit criteria**

1. The v0.1/v0.2 scenario runs with the real board in the loop.
2. Each injected fault has a test that asserts the controller's reaction.
3. Latency between a plant event and the board's response is measured, with the method and its limits documented.

**Safety note.** This milestone puts real silicon in the loop, never real machinery. The project does not support connecting any of it to operating equipment.

---

## v0.4: Level 2 and hard real time (FPGA)

**Goal.** Your firmware *is* the engine ECU: it reads crank and cam signals and sensors, and commands fuel.

**In scope.** FPGA (or MCU-timer) time and I/O backend, 60-2 crank wheel and cam signal generation, analog and digital sensor and actuator I/O, jitter measurement, a reference engine-ECU firmware that achieves crank synchronisation. The plant stays on a CPU or SoC core; the FPGA provides timing and I/O.

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

**In scope.** A generator-set and electrical-bus model (load sharing, droop, breaker states), and one PMS scenario such as a blackout and recovery sequence.

**Why separate from v0.5.** A PMS needs an electrical plant, which is a different physical model from an engine. Splitting it keeps each milestone finishable.

---

## Backlog (not scheduled)

- Aftertreatment (DOC, DPF, SCR) with soot and DEF dosing.
- Detailed thermal and air-path models.
- FMI/FMU export for co-simulation.
- Web dashboard (a trace viewer comes first).
- MISRA checking in CI, once a suitable analyser and rule-text setup is chosen.
- Python bindings for scripting scenarios.
- YAML plant profiles, loaded in `sim/` only.

## What changed from the previously published roadmap, and why

The site and the earlier README listed: v0.1 vertical slice (including Renode, six PGNs, a dashboard), v0.2 aftertreatment, v0.3 fault injection, v0.4 HIL, v0.5 second plant plus PMS.

| Change | Why |
|---|---|
| v0.1 is Level 1, SIL only; Renode moves to v0.2 | A first milestone that includes a plant, a codec, a controller, an emulator and a dashboard is not finishable. The Renode CAN question is also an open risk that should not block the first usable release |
| "Six PGNs" becomes four named PGNs | A number chosen in advance is not a requirement; messages are added when a scenario needs them |
| Dashboard moves to the backlog; a CSV trace and plotting script come first | A viewer is only worth building once there is something worth looking at beyond a console log |
| Aftertreatment moves to the backlog | Large modelling effort that does not exercise the project's core skills (control loops, protocol, timing). Revisit if users ask for it |
| Fault injection moves up to v0.3 and is paired with real hardware | Fault handling is most instructive on real silicon, and the v0.1 slice already covers the basic timeouts |
| v0.5 split into marine engine (v0.5) and electrical plant with PMS (v0.6) | The marine engine tests the plant abstraction; a PMS needs a different physical model. Doing both at once is two milestones |
| Explicit Level 1 / Level 2 | Fixes who is under test and what each message means; makes the FPGA milestone coherent |

## Public text to update when this is adopted

- The organisation site's "Honest status" paragraph still says the repository holds only a README and a license. That is out of date once the vertical slice lands.
- The site's roadmap list should follow the table at the top of this file.
- The README's earlier claim that the plant "exposes crank and cam position, sensor and actuator interfaces" describes v0.4; mark it as planned until then.

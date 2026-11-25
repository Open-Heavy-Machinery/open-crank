# Roadmap

Scope is deliberately narrow. Each milestone is small enough to finish and useful on its own. **There are no dates**: a milestone is done when its exit criteria are met, not when a calendar says so. Architecture context is in [ARCHITECTURE.md](ARCHITECTURE.md); what changed and when is in [CHANGELOG.md](CHANGELOG.md).

## At a glance

| Version | Theme | DUT level |
|---|---|---|
| **v0.1** (done) | Level 1 vertical slice, in CI | 1 |
| **v0.1.x** (done) | MISRA-oriented checks and stack usage in CI | 1 |
| **v0.2** (done) | Reference board on the bus | 1 |
| **v0.2.1** (done) | Acceptance suite over the bus | 1 |
| **v0.2.2** (done) | Board automation | 1 |
| **v0.3** | Robustness on the board | 1 |
| **v0.4** | Level 2: your firmware as the engine ECU | 2 |
| **v0.5** | Two engines on one machine | 1 |
| **v0.6** | Marine 2-stroke plant | 1 |
| **v0.7** | Electrical plant and power management | 1 |
| **Track R** | Firmware in the loop in Renode (parallel) | 1 |
| Beyond 1.0 | 3D machines (separate repos) | — |

## Definition of done (every release)

- CI is green on a clean machine.
- The README's build and run instructions work verbatim.
- `docs/parameters.md` lists the provenance of every parameter that changed.
- No claim in the docs is stronger than what the code and tests support.

---

## v0.1: Level 1 vertical slice

**Status: done.** Every exit criterion below is met, verified by the maintainer on 2026-11-18.

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

- [x] 1. **Build and tests pass in CI on a clean machine.** The workflow is in [`.github/workflows/ci.yml`](.github/workflows/ci.yml): gcc 11 and clang 14, Debug and Release, warnings as errors, sanitizers. It is green on GitHub.
- [x] 2. **Closed-loop scenarios pass**: cold start to a target speed, load step and release, loss of the speed command, loss of engine status, overspeed protection, plus an engine that ignores the override. [`tests/test_closed_loop.c`](tests/test_closed_loop.c) runs the files in [`scenarios/`](scenarios/).
- [x] 3. **`portable/` compiles for Cortex-M4F with `-Wconversion -Wdouble-promotion` as errors and needs no symbol from outside except the four that GCC requires of any freestanding environment** (`memcpy`, `memmove`, `memset`, `memcmp`). [`tools/check_portable.sh`](tools/check_portable.sh). *Wording changed: "no undefined symbols at all" would fail on legitimate struct copies (ARCHITECTURE.md D16).*
- [x] 4. **At least one deliberate "break the code" check per safety-relevant behaviour confirms that the tests can actually fail.** [`tools/mutation_check.sh`](tools/mutation_check.sh): 18 mutations, all caught.
- [x] 5. **Every plant and ECU parameter has a provenance entry**, as do the controller's and the bench's: [`docs/parameters.md`](docs/parameters.md). Many are labelled *Inherited* (source not recorded) or *Estimate*; that is honest, not final.
- [x] 6. **The `vcan` demo in the README works as written.** Run end to end on a machine with the `vcan` kernel module.

**Migration order (one commit each, each verifiable)**

As planned, with the adjustments noted.

1. Move files to `portable/`, `sim/`, `platforms/` with no logic change; then remove the leftover copy of the Linux controller from `portable/`. *Why first:* a pure move is easy to review and keeps `git blame` meaningful.
2. Separate plant physics from the simulated engine ECU. *Why:* biggest conceptual fix; everything after assumes it. *Note:* in this commit the SIL ignored TSC1 (the engine just idled); it was wired again in step 3.
3. TSC1 with its standard meaning, the portable codec, the controller core and the bench. *Why:* follows from step 2; the controller now asks for speed instead of commanding fuel.
4. Closed-loop tests on an in-memory bus. *Why:* they pin the behaviour down before the next refactors can break it. *Note:* the scenario parser arrived here, with scenarios embedded in the test.
5. Scenario files, drivers (free-run, paced) and trace. *Why:* needed by v0.2 and by CI speed.
6. CI with the portability and mutation checks. *Why last:* it enforces what steps 1 to 5 established.

---

## v0.1.x: MISRA-oriented checks and stack usage in CI

**Status: done** (2026-11-20, released as v0.1.1). Both exit criteria are met: CI is green on `main`, and the 28 findings left in the baseline are covered by four approved deviations.

**Goal.** CI checks `portable/` against part of MISRA C and reports how much stack each of its functions needs.

**In scope**

- A **MISRA-oriented check** of `portable/` in CI, with the free MISRA addon of cppcheck. Findings are reported by rule number only, without rule texts: those are copyrighted.
- `-fstack-usage` in the Cortex-M4F build, with the maximum stack usage of each function in the CI log.

**Why "MISRA-oriented".** The addon covers only part of the rules. Passing it **does not imply MISRA compliance**.

**Approach: baseline first.** The CI job starts by accepting the findings that exist today, which are stored in a baseline ([`tools/misra-baseline.txt`](tools/misra-baseline.txt), counts per rule and file). From then on it fails if a new one appears. As the maintainer fixes findings or documents them as deviations, the baseline goes down. CI protects `portable/` from the first day, and the fixes come after. Exit criterion 2 is met when the baseline contains only findings documented as deviations.

**Outcome.** 21 of the 49 initial findings were fixed in the code. The remaining 28 are covered by four deviations in [`docs/misra_deviations.md`](docs/misra_deviations.md), each naming the baseline pairs it covers. The stack usage of every function in `portable/` is printed by [`tools/check_portable.sh`](tools/check_portable.sh), which also fails if any function is not `static`.

**Exit criteria**

- [x] 1. **The CI job is green.** The `misra` job and the Cortex-M4F stack report pass on `main`.
- [x] 2. **Every finding is either fixed or documented as a deviation, with its reason.** [`docs/misra_deviations.md`](docs/misra_deviations.md): DEV-001 to DEV-004, approved by the maintainer.

---

## v0.2: Reference board on the bus

**Goal.** The reference controller, unchanged, runs on a cheap reference board and regulates the simulated engine over a real CAN link.

**In scope**

- **Reference board**: an STM32G431 (Nucleo-32 class), a CAN transceiver and a USB-CAN adapter that works with SocketCAN, with a bill of materials.
- **A bring-up guide** that starts from scratch.
- `platforms/stm32g4`, running `portable/machine_ctrl` unchanged.
- **The platform/core boundary, documented** for porting to other boards: pass each received frame to `mc_handle_frame`, call `mc_step(now_ms)` on every tick, and send the frames it returns. The API already works this way; it only needs documenting.
- Ports to bxCAN and other families are welcome as community contributions. They are not a goal of the project.
- Plotting CSV traces to compare the board against the SIL: [open-rig](https://github.com/Open-Heavy-Machinery/open-rig) draws them with [`viewer.json`](viewer.json), and [`tools/compare_runs.py`](tools/compare_runs.py) checks the tolerances.

**Exit criteria**

1. **Latency first, then tolerances, then the comparison.** Measure the latency of the USB-CAN adapter first. With that figure, write down the tolerances *before* comparing: maximum speed droop, recovery time, steady speed. Then `scenarios/hoist_step.csv` with the board in the loop must stay within those tolerances of the reference. The reference is the SIL paced by the wall clock (`open-crank-sil` with `open-crank-mc`), not the free-running tests.
2. **The guide has been followed from scratch**, on a clean clone. Validation by a third party is recorded when it happens and does not block the release.
3. **The worst observed execution time of `mc_step`** (measured with the DWT cycle counter; not a proven bound) and its stack usage are documented. The stack budget uses the call graph (`-fcallgraph-info=su`), with the CI's `arm-none-eabi-gcc` as the reference toolchain.

**Met on 2026-11-22.** 1: [docs/board-vs-sil.md](docs/board-vs-sil.md#results), three board runs within the tolerances. 2: [platforms/stm32g4/BRINGUP.md](platforms/stm32g4/BRINGUP.md#followed-from-scratch), followed from scratch by the maintainer. 3: [execution time](platforms/stm32g4/README.md#execution-time-of-mc_step) and [stack usage](platforms/stm32g4/README.md#stack-usage-of-the-controller) in the STM32G4 README.

**Safety note.** From this milestone on, real silicon is in the loop, never real machinery. The project does not support connecting any of it to operating equipment.

---

## v0.2.1: Acceptance suite over the bus

**Goal.** Grade **any** device under test by its CAN frames alone, without knowing how it is implemented.

**In scope.** A runner that plays scenarios with the bench over SocketCAN (`vcan` or a USB-CAN adapter) and grades the device under test only by its frames. Each scenario carries its own pass/fail criteria, written only in terms of what is observable on the bus, with reaction times measured from the injected event. For example:

- within T ms of losing EEC1, TSC1 is sent with override disabled and OC_PUMP_CMD displacement is 0;
- in every TSC1 frame of the scenario, the requested speed never exceeds the engine's high idle.

The runner produces a pass/fail report.

Once proven here, the oracle moves to open-tick (its v0.2), as the runtime did in v0.1.

**Exit criteria**

1. The reference controller passes, on `vcan` and on the reference board.
2. A deliberately broken controller fails.

**Met on 2026-11-23.** 1: [docs/acceptance.md](docs/acceptance.md#results), the reference controller passes all seven scenarios on `vcan` (in CI) and on the reference board. 2: [docs/acceptance.md](docs/acceptance.md#deliberately-broken-controllers), six mutated controllers fail by the expected criteria in CI; one accepted survivor, with its reason.

---

## v0.2.2: Board automation

**Goal.** The reference board is built, programmed and tested from the PC with one command, without STM32CubeIDE and without pressing a button, so that v0.3's fault scenarios can be run on the board as often as in SIL.

**In scope**

- The board firmware (the reference controller and the round-trip echo) built with CMake and a pinned `arm-none-eabi-gcc`, from the repository alone: clock, FDCAN, HAL configuration and linker script written here; ST's CMSIS, start-up file and HAL downloaded from STMicroelectronics' repositories at pinned versions, never copied into the repository.
- Programming and reset through the Nucleo's ST-LINK with OpenOCD, and a setup script that gets a Linux PC ready.
- A standard bench check, one command: it measures the bus round trip with the echo firmware, then plays every acceptance scenario on the board, resetting it before each one and checking that it is alive and not in FAULT before grading, and that the board runs the firmware built from the current commit; it writes one report that compares the bench with the reference bench.
- STM32CubeIDE stays as the entry point for beginners ([BRINGUP.md](platforms/stm32g4/BRINGUP.md)) and as a reference for applications that depend on it; test automation does not use it.

**Exit criteria**

1. The board firmware builds in CI from the repository alone (CMake, pinned toolchain, ST's CMSIS and HAL fetched at pinned versions, no CubeMX-generated code), and its size and stack figures come from CI.
2. One command on the PC programs the board, checks that it runs the firmware built from the current commit, measures the bus round trip, and runs the seven acceptance scenarios with a software reset before each and a check that the device under test is alive and not in FAULT: 0 echoes lost, seven PASS, 0 late steps, every reaction time within one 10 ms step of v0.2.1's board run. *(Within one step, not equal: a reaction lands in the same 10 ms step as on the reference or the next one, depending on where the board's tick falls against the bench's after a reset.)*
3. [BRINGUP.md](platforms/stm32g4/BRINGUP.md) stays the entry point for beginners and says that test automation does not use it.

**Met on 2026-11-24.**

1. CI's `firmware` job builds `oc-stm32g4-demo` and `oc-stm32g4-echo` with the pinned Arm GNU Toolchain 14.2.rel1 and checks that no CubeMX-generated function is in them. The `portable-cortex-m4f` job gives the stack figures with the same toolchain (`mc_handle_frame` 96 B, `mc_step` 68 B). See [platforms/stm32g4/README.md](platforms/stm32g4/README.md#build-and-program-without-cubeide).
2. `tools/board/bench.sh` on the reference bench, on this release's commit, gave BENCH OK:
   - round trip 1000 of 1000, p99 1.895 ms;
   - seven PASS, 0 late steps;
   - every reaction within one step of v0.2.1's board run.

   See [platforms/stm32g4/README.md](platforms/stm32g4/README.md#the-bench-check). The report and the traces are to be attached to the GitHub release of v0.2.2.
3. [BRINGUP.md](platforms/stm32g4/BRINGUP.md) opens with "Two ways in".

---

## v0.3: Robustness on the board

**Goal.** Failures are testable on the reference board, and the controller's reactions to them are specified and traced to tests.

**In scope**

- **Fault injection**: lost frames, jitter, corrupted frames, bus-off.
- **Sensor plausibility faults reported by the engine (DM1) and the controller's limp-home reaction.**
- **DM1**: the simulated engine sends it, and the controller reads it and reacts (core). The controller sending DM1 for its own faults is an optional part of the reference firmware.
- **Watchdog with a defined state after reset**: override released and pump command at zero until the engine has been confirmed again.
- **Black box and configuration loaded from EEPROM**, as an optional part of the reference firmware.
- **Numbered safety requirements**, each traced to its tests.
- **A general specification of the safe state.**

Transport-level faults are built in open-tick (its v0.3); domain faults stay here.

Fault injection pulls in protocol work that v0.1 left out on purpose:

- TSC1 message counter and checksum (SPN 4206/4207), so that corrupted and replayed frames can be rejected.
- DM1 (active diagnostic trouble codes) for faults reported by the engine; with more than one active code, DM1 needs the J1939-21 transport protocol (BAM) on the sending side.

**Status: in progress, in three steps.**

1. **Done (open-tick v0.3, step 1):** links and transport faults in open-tick's core.
2. **Done in SIL and on `vcan` (step 2, #33):**
   - the bench has two links, `mc` and `engine`;
   - four fault scenarios: `engine_status_delay`, `engine_status_jitter`, `speed_command_intermittent`, `mc_bus_off`;
   - the safe state and the requirements `SR-1`…`SR-9` in [`docs/safety.md`](docs/safety.md), checked in CI by `tools/check_requirements.py`.
3. **In progress, v0.3.1, on the board** (steps 3a to 3d):
   - *done (step 3a):* the fault scenarios in `tools/board/bench.sh`, with board reference times;
   - 3b: the TSC1 counter and checksum (SPN 4206/4207), with a `corrupt` scenario;
   - 3c: the watchdog, after whose reset the controller starts in FAULT and waits for the operator's fault reset;
   - 3d: DM1 from the engine (read with BAM when more than one code is active), and limp-home: requested speed at most 1200 rpm and pump at most 50 %, reached smoothly, while the amber lamp is on.
   - The controller's own DM1 is left for after v0.3.1.

**Exit criteria**

1. Each injected fault has a test that verifies the reaction.
   - *Met in SIL and on `vcan`* for the faults injected so far: lost frames (`fault.mc_tx_lost`, `fault.engine_tx_lost`, the links' `drop_one_in`), delay, jitter and bus-off ([`docs/acceptance.md`](docs/acceptance.md), "Link faults").
   - *Open:* `corrupt` and the DM1 faults (v0.3.1), and the board (v0.3.1).
2. Each safety requirement is linked to at least one test.
   - *Met:* `SR-1`…`SR-9`, each cited by an acceptance criterion or a unit test ([`docs/safety.md`](docs/safety.md)); `tools/check_requirements.py` fails CI otherwise. Requirements added in v0.3.1 must keep it so.

---

## v0.4: Level 2, your firmware as the engine ECU

**Goal.** Your firmware *is* the engine ECU: it reads crank and cam signals and sensors, and commands fuel.

**Depends on** [open-tick](https://github.com/Open-Heavy-Machinery/open-tick) v0.4. Its Tang Nano 20K bridge provides the signal-level transports: tooth-wheel patterns for crank and cam, injector capture and analog outputs. This milestone provides the engine side: the crank-angle plant and the reference engine-ECU firmware.

**In scope**

- The simulated engine ECU is switched off; the plant's fuel comes from the captured injector pulses.
- A plant step of about 1 ms. The plant gains crank angle over a full 720° cycle here, not before.
- Signals: 60-2 crank wheel, cam, analog sensors (PWM with an RC filter, or an external DAC) and injector capture.
- The signal generator is an FPGA, the Tang Nano 20K, through open-tick's bridge. A second MCU was the cheap alternative: a G431's timers give sub-microsecond resolution, but their limit is the number of channels. Level 2 needs many independent timed channels at once (crank, cam, one capture per injector, analog outputs, room for a second engine), which is what an FPGA is for. The Raspberry Pi Pico bridge stays the cheap option for machines with fewer channels, such as open-robots.

The plant stays on a CPU; the signal generator provides timing and I/O.

**Exit criteria**

1. A jitter budget is stated and measured.
2. A reference engine-ECU firmware achieves crank synchronisation and holds a requested speed.

---

## v0.5: Two engines on one machine

**Goal.** One bench runs two engines on the same machine, each with its own J1939 source address.

**In scope**

- Two instances of `d936la6` on the same machine. This generalises the bench, **not** the plant.
- Distinct J1939 source addresses.
- J1939-81 address claiming, and behaviour on address conflicts.
- Check whether, on a real crane, both engines share one J1939 segment.

The machine is described as an open-tick profile (its v0.5) instead of a hand-built bench.

**Exit criteria**

1. Both engines run on the same runtime.
2. A fault on one engine does not affect the other.
3. There is a test for address claiming.

---

## v0.6: Marine 2-stroke plant, and extracting the plant interface

**Goal.** Prove the architecture generalises by adding a radically different engine, then extract the plant interface from what both plants actually share.

**In scope.** `9s50me`: 2-stroke marine engine with a propeller-law load and a Level 1 control interface of its own, ME-style (not J1939, which is not how that engine is controlled). The plant plug-in interface is extracted here, not before.

**Exit criteria**

1. Both plants run on the same runtime and the same scenario runner.
2. A written list of what the second plant forced to change in the core, and why.

---

## v0.7: Electrical plant and power management

**Goal.** Make power-management development possible.

**In scope.** A generator-set and electrical-bus model (load sharing, breaker states), and one PMS scenario such as a blackout and recovery sequence. It builds on v0.5, which lets one bench run several engines: here, the generator sets. The engine ECU's droop setting, present since v0.1, is what load sharing between sets builds on.

**Why separate from v0.6.** A PMS needs an electrical plant, which is a different physical model from an engine. Splitting it keeps each milestone finishable.

---

## Track R: Firmware in the loop in Renode (parallel)

**Goal.** Firmware in the loop inside CI: the board's firmware, run in an emulator, regulates the simulated engine.

**When.** A parallel track. It can be done at any time after v0.2.

**Where.** The virtual-time driver that steps the simulation in lockstep with the emulator is open-tick's (its Track R). This track keeps what is specific to the board: the G4 platform description and the comparison with the board's trace.

**In scope**

- For the G4: a platform description (`.repl`) of our own, using Renode's `CAN.STM32_FDCAN` model, the same one that `platforms/cpus/stm32l552.repl` uses. It is **assumed** to be the same IP as the G4's FDCAN; that still has to be verified.
- Fallback: an STM32H7 with `CAN.MCAN`, which Renode supports officially.
- A virtual-time driver that steps the bench in lockstep with the emulator.

**Exit criteria**

1. The trace in Renode matches the board's within the stated tolerance.
2. It runs headless in CI.

For anyone without a board, the free path is the SIL.

---

## Beyond 1.0: 3D machines (separate repositories)

No commitment to dates.

- 3D machines, such as a crane in Gazebo or O3DE with ROS 2, in separate repositories: a bridge and a generic crane description, with no brand and no manufacturer meshes.
- Rule: every signal has exactly one owner, simulated or real.

---

## Backlog (not scheduled)

- Power limiting / anti-stall in the reference controller (the bus interface already lets the controller act on its load).
- Speed-dependent turbocharger model; full engine-torque-mode reporting (SPN 899).
- Aftertreatment (DOC, DPF, SCR) with soot and DEF dosing.
- Detailed thermal and air-path models.
- FMI/FMU export for co-simulation.
- Web dashboard (a trace viewer comes first).
- Python bindings for scripting scenarios.
- YAML plant profiles, loaded in `sim/` only.

## Public text to update

- open-crank's page on the organisation site ([open-heavy-machinery.github.io/open-crank](https://open-heavy-machinery.github.io/open-crank/)) follows the table at the top of this file, and the site's home page summarises it. Change them together.
- The organisation's mission, used in this README, the site and the organisation profile: "Open tools to practise and develop embedded control for machines you cannot keep at home, starting with heavy machinery."
- An earlier README said the plant "exposes crank and cam position, sensor and actuator interfaces". That describes v0.4; the current README marks Level 2 interfaces as planned. Any copy of the old text elsewhere should be corrected.

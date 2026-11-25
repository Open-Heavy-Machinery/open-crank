# Safety requirements and the safe state

This page says what the reference machine controller (`portable/machine_ctrl`) must do to be safe, and which tests check it. It is the specification that open-crank v0.3's exit criterion 2 refers to: each requirement is linked to at least one test.

It covers the reference controller on the reference bench (`OC_BENCH_D936LA6`, `MC_DEFAULT_PARAMS`). The numbers come from [`parameters.md`](parameters.md). It is not a hazard analysis of a real machine and claims no compliance with a functional-safety standard. A real machine needs its own hazard analysis, and its numbers may differ.

## The safe state

**What it is.** The controller asks nothing of the engine or of the hoist:

- **override released**: TSC1 with override mode `00` (override disabled), or no TSC1 for more than 200 ms. The engine then falls back to its own governor (low idle);
- **pump at 0**: OC_PUMP_CMD with displacement 0, or no OC_PUMP_CMD for more than 200 ms. A stale command counts as zero, as a de-energised proportional solenoid would.

Silence counts as releasing because that is what the machine does with a stale command: the engine drops a TSC1 older than its 200 ms request timeout, and the pump takes a command older than 200 ms as zero. This is the same rule the acceptance oracle uses ([`acceptance.md`](acceptance.md), "Silence counts as releasing"). A TSC1 that keeps any other override mode is not a release, even without a valid requested speed.

**When the controller is in it.** In every state except RUN:

- **WAIT_ENGINE**, after power-up and after a fault reset, until the engine is confirmed running: valid EEC1 with engine speed at or above `running_rpm` (600 rpm) for `run_confirm_ms` (500 ms);
- **FAULT**, entered from RUN when it loses the engine status, when the engine does not honour the override, on overspeed or on stall.

**How it leaves it.** From FAULT only through a **fault reset**, a new press of the operator's button (holding it does not count), which takes the controller to WAIT_ENGINE. From there it enters RUN only when the engine is confirmed running again. There is no automatic recovery: frames flowing again after a loss do not clear a fault.

*(planned, v0.3.1)* The same state after a watchdog reset of the board: the controller starts in WAIT_ENGINE, so it already sends the safe state until the engine is confirmed, but the board has no watchdog yet.

## Requirements

Each requirement has a sentence, its reason and the tests that check it. An acceptance criterion that checks one starts its text with the requirement's number, as `SR-4: …` or `SR-4, SR-7: …` ([`acceptance.md`](acceptance.md)). A unit test that checks one carries the comment `/* SR-4 */` just before its function. [`tools/check_requirements.py`](../tools/check_requirements.py) fails, in CI, if a requirement below is checked by no criterion and no unit test, or if a criterion or a test cites a requirement that does not exist.

### SR-1: No override before the engine is confirmed running

The controller requests no override (TSC1 mode `00`) and commands the pump to 0 until the engine has been confirmed running (WAIT_ENGINE above).

**Why.** While the engine is stopped or cranking, a speed request fights the ECU's start-up logic, and hoist motion with the engine not yet running cannot be sustained.

Checked by: criterion S1 in every scenario; unit tests `test_waits_for_engine`, `test_run_after_confirmation`.

### SR-2: Requested speed never above high idle

The requested speed is never above the governed ceiling, 2100 rpm (`request_max_rpm`), whatever the operator asks for.

**Why.** The ceiling protects the engine and the hydraulics. An operator target or a corrupted input must not ask for more.

Checked by: criterion G1 in every scenario; unit test `test_request_clamped`.

### SR-3: The pump is commanded only while in speed control

A displacement above 0 is commanded only while the controller is in RUN and holds the override.

**Why.** The hoist load is sized for the governed speed under control. Outside RUN the controller does not know that the engine can carry it, and a load on a stalling or uncontrolled engine is not safe.

Checked by: criterion G2 in every scenario (with a 20 ms tolerance for the order of the two frames); unit test `test_hoist_only_in_run`.

### SR-4: Release when the engine status is lost or late

When no valid EEC1 has arrived for more than `eec1_timeout_ms` (200 ms), the controller enters FAULT and the safe state, within 300 ms of the moment EEC1 stops arriving.

**Why.** Without a fresh engine speed the controller cannot detect overspeed or stall, so it must not keep asking for speed or load. A status that arrives late is as bad as none: the first 300 ms of a 300 ms delay bring nothing.

Checked by: criteria E2 (`engine_status_loss`) and D1 (`engine_status_delay`); unit test `test_engine_status_timeout`; closed-loop test `test_engine_status_loss`.

### SR-5: Release when the engine does not honour the override

When EEC1 (SPN 1483, source address of the controlling device) has not shown this controller in control for `override_confirm_ms` (500 ms) while in RUN, the controller enters FAULT and the safe state, within 1 s of the engine no longer honouring it.

**Why.** If the engine ignores the speed request, the controller's view of the machine is wrong: commanding load on a speed it does not control is unsafe. This is also how the controller learns that its own frames are not arriving, whatever the cause: a lost path, a bus-off, a broken ECU.

Checked by: criteria O1 (`override_ignored`), L1 (`speed_command_loss`), B1 (`mc_bus_off`) and K1 (`speed_command_corrupt`); unit test `test_override_must_be_honoured`; closed-loop tests `test_speed_command_loss`, `test_override_ignored`.

### SR-6: Release on overspeed

When the engine speed in EEC1 exceeds `overspeed_rpm` (2300 rpm) while in RUN, the controller enters FAULT and the safe state, within 1 s of the overrun that causes it.

**Why.** An overrunning load can drive the engine above its governed range. The controller must stop adding load and hand the engine back to its own governor and its overspeed trip (2400 rpm).

Checked by: criterion V1 (`overspeed`); unit test `test_overspeed_and_stall`; closed-loop test `test_overspeed_protection`.

### SR-7: A fault latches until a fault reset

Once in FAULT, the controller stays in the safe state until a new press of the fault reset, and then re-enters RUN only through WAIT_ENGINE.

**Why.** A cause that comes and goes (an intermittent connector, a delay that clears) must not make the machine resume on its own: the operator must see that something happened and decide to continue.

Checked by: criteria E2, D1, O1, L1, B1, K1 and V1, which require the safe state until the fault reset or the end of the scenario, including after the cause is gone in `engine_status_delay`, `speed_command_loss` and `mc_bus_off`; unit test `test_reset_on_press_only`.

### SR-8: Releasing the hoist lever stops the pump

When the operator releases the hoist lever, the pump command falls to 0 within 100 ms.

**Why.** The lever is the operator's first way to stop the motion.

Checked by: criterion H3 (`hoist_step`).

### SR-9: Transport disturbances within the timeouts do not trip the controller

Engine status that arrives up to 150 ms late with jitter, or the loss of one in three of the controller's frames, does not make the controller fault: it keeps speed control and the pump command.

**Why.** A controller that faults on every late or lost frame stops the machine for no reason, and an operator who keeps resetting false faults stops trusting real ones. The margins come from the timeouts: 150 ms is below the 200 ms EEC1 timeout, and with one frame in three lost the engine's 200 ms request timeout never expires when TSC1 is sent every 10 ms.

Checked by: criteria J1 (`engine_status_jitter`) and I1 (`speed_command_intermittent`).

### SR-10: Every TSC1 carries a valid counter and checksum

Every TSC1 the controller sends carries a message counter that goes up by one per frame (SPN 4206, 15 wraps to 0) and a message checksum (SPN 4207), as J1939-71 defines them ([`bus-interface.md`](bus-interface.md)).

**Why.** A frame can pass the CAN CRC with the wrong content: a faulty gateway, a sender's memory or a bit flipped after the CRC check. Without the checksum, an engine reads a speed request whose high byte is flipped as about 6700 rpm, and goes to its 2100 rpm ceiling. With it, the engine refuses the frame as if it were lost, and SR-5 releases. The counter lets the engine refuse a frozen sender or a repeated frame.

Checked by: criterion G3 in every scenario, through the observable `tsc1_intact` (checksum right, and counter = previous + 1); criterion K1 (`speed_command_corrupt`), through SR-5; unit tests `test_tsc1_counter_and_checksum` (codec), `test_tsc1_integrity_checked` (the simulated engine refuses) and `test_accept_tsc1_intact` (the observable); closed-loop test `test_speed_command_corrupt` (the engine stays below 1600 rpm while every TSC1 is corrupted).

## What is not specified yet

- **Stall**: the controller enters FAULT below `stall_rpm` (unit test `test_overspeed_and_stall`), but no scenario drives the engine into a stall under control, so there is no acceptance criterion and no requirement for it.
- **Engine diagnostics (DM1) and limp-home** *(planned, v0.3.1)*.
- **Watchdog** *(planned, v0.3.1)*: see the safe state above.

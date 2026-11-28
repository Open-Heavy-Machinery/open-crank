# MISRA C:2012 deviations for `portable/`

This file documents every finding that `tools/check_misra.sh` still reports and that is kept on purpose. A finding is either fixed in the code or recorded here, with its scope, its reason and how the risk is controlled.

**MISRA-oriented, not compliant.** The check runs cppcheck's MISRA addon, which covers only part of the guidelines. Passing it, with every remaining finding documented here, does not make `portable/` MISRA compliant. Rules are named by number and category only: the guideline texts are copyrighted and are not reproduced in this repository.

**Reference toolchain.** The findings are those of Cppcheck 2.19.0 on `ubuntu-26.04`, as installed by the CI `misra` job and by `tools/check_misra_docker.sh`. They are checked with `--platform=arm32-wchar_t4`.

## How this file and the baseline fit together

`tools/misra-baseline.txt` lists the findings CI accepts, as counts per (rule, file). Every pair in it must be covered by a deviation below, and every deviation names the pairs it covers. `tools/check_misra.sh` enforces this in CI: each baseline pair needs a row in the table below with the same count, and its deviation must have the status "approved by the maintainer".

- **To fix a finding:** change the code, run `tools/check_misra_docker.sh --update`, and remove the finding from its deviation's scope here.
- **To keep a new finding:** add or extend a deviation here first, then update the baseline. A finding that is not covered here must not enter the baseline.
- **Traceability in the code:** each early return kept under DEV-003 or DEV-004 carries a `/* DEV-00n: ... */` comment on the line cppcheck reports.

| Baseline pair (rule, file) | Count | Deviation |
|---|---|---|
| 8.7, `portable/j1939/src/j1939.c` | 5 | DEV-001 |
| 8.7, `portable/j1939/src/oc_msgs.c` | 2 | DEV-001 |
| 8.7, `portable/machine_ctrl/src/machine_ctrl.c` | 6 | DEV-001 |
| 8.7, `portable/engine_ecu/src/ecu.c` | 9 | DEV-001 |
| 2.5, `portable/j1939/include/opencrank/j1939.h` | 1 | DEV-002 |
| 2.5, `portable/j1939/include/opencrank/oc_msgs.h` | 1 | DEV-002 |
| 15.5, `portable/j1939/src/j1939.c` | 4 | DEV-003 |
| 15.5, `portable/j1939/src/oc_msgs.c` | 2 | DEV-003 |
| 15.5, `portable/machine_ctrl/src/machine_ctrl.c` | 2 | DEV-004 |

All four rules are advisory.

---

## DEV-001: Public API functions called from outside the analysed code

- **Rule:** MISRA C:2012 Rule 8.7 (Advisory): limiting external linkage to what is referenced from more than one translation unit.
- **Scope:** the 22 functions declared in the public headers `portable/j1939/include/opencrank/j1939.h`, `oc_msgs.h`, `portable/machine_ctrl/include/opencrank/machine_ctrl.h` and `portable/engine_ecu/include/opencrank/ecu.h` that cppcheck reports:
  - `portable/j1939/src/j1939.c`: `j1939_id_priority`, `j1939_encode_et1`, `j1939_decode_et1`, `j1939_encode_eflp1`, `j1939_decode_eflp1`
  - `portable/j1939/src/oc_msgs.c`: `oc_encode_operator`, `oc_decode_pump_cmd`
  - `portable/machine_ctrl/src/machine_ctrl.c`: `mc_init`, `mc_handle_frame`, `mc_clear_fault`, `mc_step`, `mc_state_name`, `mc_fault_name`
  - `portable/engine_ecu/src/ecu.c` (since v0.4, the reference engine ECU; extended with the maintainer's approval, 2026-11-28): `ecu_wheel_tooth`, `ecu_wheel_cam`, `ecu_wheel_poll`, `ecu_wheel_set_width`, `ecu_wheel_status`, `ecu_init`, `ecu_handle_frame`, `ecu_step`, `ecu_width_ticks`
  - `j1939_decode_tsc1` and `j1939_encode_eec1` left this list in v0.4: the engine ECU calls them.
- **Justification:** `portable/` is a pair of libraries. Their purpose is to be linked with code that the check does not analyse: the simulator (`sim/runtime/src/bench.c`, `loop.c`, `trace.c`), the platforms (`platforms/host/run_main.c`, `platforms/linux-socketcan/mc_main.c`), the tests, from v0.2 the board firmware and anyone's own firmware, and from v0.4 the engine ECU's board firmware and the in-memory Level 2 loop *(planned)*. cppcheck sees only `portable/`, so a function that the library itself does not call looks as if it had a single user. Giving these functions internal linkage would remove the API.
- **Risk and mitigation:** the risk the rule addresses, an identifier visible more widely than needed, does not apply to an interface that exists to be visible. Each function has a prototype in a public header, is documented there, and is exercised by `tests/test_j1939.c`, `tests/test_machine_ctrl.c` or `tests/test_ecu.c`. Functions that are not part of the API stay `static` (for example `clampf`, `override_rejected`, `enter_run`).
- **Status:** approved by the maintainer.

## DEV-002: Public macros used outside the analysed code

- **Rule:** MISRA C:2012 Rule 2.5 (Advisory): macro definitions that the analysed code does not use.
- **Scope:** two macros in the public headers:

  | Macro | Header | Used by |
  |---|---|---|
  | `J1939_ADDR_NULL` | `j1939.h` | nobody yet; J1939-81 address claiming, open-crank v0.5 |
  | `OC_ADDR_BENCH` | `oc_msgs.h` | `sim/runtime/src/bench.c`, `tests/test_j1939.c`, `tests/test_machine_ctrl.c` |

- **Justification:** these are named values of the bus contract ([`docs/bus-interface.md`](bus-interface.md)). They are offered to code that builds or reads frames, which here is the simulator, the tests or a user's firmware, and not to `portable/` itself. `J1939_ADDR_NULL` is kept by the maintainer's decision as part of the J1939 vocabulary the API offers, ahead of address claiming in v0.5. Private macros that nobody used (`J1939_U8_NA`, `J1939_U16_NA` in `j1939_scaling.h`) were removed instead of deviated.
- **Risk and mitigation:** an unused macro has no effect on the generated code. The macro in use carries the value documented in `docs/bus-interface.md` (bench address `0xFC`) and is checked by known test vectors in `tests/test_j1939.c`. The two SPN 899 torque modes (`0` and `0xF`) left this list in v0.4: the engine ECU uses them. `J1939_ADDR_NULL` (`0xFE`) is not used yet; its documentation and tests come with address claiming in v0.5.
- **Status:** approved by the maintainer

## DEV-003: Early-return guard clause in message decoders

- **Rule:** MISRA C:2012 Rule 15.5 (Advisory): a single point of exit at the end of a function.
- **Scope:** strictly limited to the PGN validation guard (`if (!j1939_frame_is(...)) { return false; }`) at the entry of the six stateless decoder functions:
  - `portable/j1939/src/j1939.c`: `j1939_decode_tsc1`, `j1939_decode_eec1`, `j1939_decode_et1`, `j1939_decode_eflp1`
  - `portable/j1939/src/oc_msgs.c`: `oc_decode_operator`, `oc_decode_pump_cmd`
- **Justification:** these decoders are pure functions that operate only on caller-provided buffers within `portable/`. They use no hardware resource, no dynamic memory, no mutexes and no shared state. Exiting early before the output struct is written makes them easier to read and avoids nesting the whole decoding body.
- **Risk and mitigation:** the risk is negligible: nothing is acquired before the exit, and the output structure is left untouched when the PGN or the length does not match. This is verified by `test_rejects_short_and_foreign_frames` in `tests/test_j1939.c`, which feeds wrong-PGN and short frames.
- **Status:** approved by the maintainer (Cppcheck 2.19.0).

## DEV-004: Early returns in the override confirmation (`override_rejected`)

- **Rule:** MISRA C:2012 Rule 15.5 (Advisory): a single point of exit at the end of a function.
- **Scope:** the two early `return false` in `override_rejected` (`portable/machine_ctrl/src/machine_ctrl.c`), a `static` function of eleven lines with three outcomes:
  1. the engine reports this controller in control (SPN 1483): clear the mismatch and return;
  2. a mismatch is seen for the first time: start timing it and return;
  3. otherwise: report whether the mismatch has lasted longer than `override_confirm_ms`.
- **Justification:** each early return ends one of the three outcomes of a small decision, in the order in which the controller reasons about it. With a single exit, the timing of outcome 3 would sit inside nested conditions, which is harder to review in safety-relevant logic than three flat, separate cases. The maintainer chose to keep the function as it is.
- **Risk and mitigation:** the function acquires nothing and changes only the controller context it is given, before each return. Its behaviour is pinned down three ways:
  - `tests/test_machine_ctrl.c` checks the override confirmation;
  - the closed-loop scenario `scenarios/override_ignored.csv` (`tests/test_closed_loop.c`) exercises it;
  - the mutation "controller: never checks that the engine honours the override" (`tools/mutation_check.sh`) fails the suite if the check is bypassed.
- **Status:** approved by the maintainer

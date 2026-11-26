# Bus interface (Level 1)

This is the contract between the simulated machine and a **Level 1 device under test**: a machine controller that asks the engine for speed and reads its status. If you write your own controller, this page is all it needs to know. The reference implementation is [`portable/machine_ctrl`](../portable/machine_ctrl).

Everything travels as 29-bit CAN frames with J1939 framing, so the same controller works against the SIL on `vcan`, in an emulator, and (from v0.2) on a real board.

**Machine-readable form: [`open-crank.dbc`](../open-crank.dbc).** Every message below, with its signals, scaling, value tables and period (`GenMsgCycleTime`), as a DBC for cantools, SavvyCAN or Vector tools (`ProtocolType "J1939"`, `VFrameFormat` J1939PG; identifiers with the nominal source addresses). **This page is the contract; the DBC must agree with it**, and CI checks that it agrees with the code that puts the frames on the bus: `tests/dbc_vectors.c` encodes frames with open-crank's encoders and `tools/check_dbc.py` decodes them with the DBC, signal by signal (`tools/check_dbc.sh`, which also checks that seven deliberate errors in the DBC are caught). DBC cannot describe J1939 transport: a DM1 with two codes or more travels as a BAM (TP_CM_BAM, TP_DT), and a reader reassembles it itself.

> **Verify against your copy of SAE J1939-71.** The J1939 layouts below are original work from public sources. The repository ships no spec text.

## Bus

Classical CAN (CAN 2.0B, not CAN FD), 29-bit identifiers, 8-byte data field, J1939-21 framing. On a real bus, 250 kbit/s, the rate of the J1939-11 physical layer. On `vcan` the bit rate does not apply.

With a slcan USB-CAN adapter:

```sh
sudo slcand -o -c -s5 /dev/serial/by-id/<your-adapter> can0 && sudo ip link set can0 up
```

## Nodes and addresses

| Node | Source address | Sends | Notes |
|---|---|---|---|
| Engine (simulated engine ECU) | `0x00` | EEC1, ET1, EFL/P1 | Engine #1. Silent while the key is off. |
| Machine controller (device under test) | `0x27` | TSC1 (to `0x00`), OC_PUMP_CMD | Default of the reference controller; the engine accepts TSC1 from any address. |
| Bench (operator panel, environment) | `0xFC` | OC_OPERATOR | Always on. |

## Messages

Byte numbers are **0-based** (J1939 documents count from 1). Unused bytes and bits are `0xFF` / `1`. One-byte values `0xFB`–`0xFF` and two-byte values above `0xFAFF` mean "error" or "not available" and must not be read as numbers.

### Bench test frame (bench → board, test only)

`0x18FF2DFC` (PGN `0xFF2D`, from the bench, `0xFC`), 8 bytes of `0xFF`. The bench sends it every step while the scenario signal `test.dut_hang` is 1. Only the hang-test build `oc-stm32g4-demo-hangtest` reacts: it stops its control loop, so that the watchdog can be tested (`tools/board/watchdog_check.sh`). The reference controller and any other device under test ignore it.

### TSC1, Torque/Speed Control 1 (PGN 0, to the engine), every 10 ms while overriding

| Byte | Content | Scaling |
|---|---|---|
| 0, bits 0-1 | Override control mode (SPN 695): `00` disabled, `01` speed control, `10` torque control, `11` speed/torque limit | |
| 0, bits 2-3 | Requested speed control conditions (SPN 696) | ignored by the simulated engine |
| 0, bits 4-5 | Override control mode priority (SPN 897), `00` highest .. `11` lowest | ignored by the simulated engine |
| 1-2 | Requested speed (SPN 898), little-endian | 0.125 rpm/bit |
| 3 | Requested torque (SPN 518) | 1 %/bit, offset −125 % |
| 7 | Message counter (SPN 4206) in bits 0-3, message checksum (SPN 4207) in bits 4-7 | **required since v0.3.1.** The counter goes up by 1 on every TSC1 you send, and 15 wraps to 0. The checksum is `((sum >> 4) + sum) & 0x0F`, where `sum` adds data bytes 0-6, the counter and the four bytes of the 29-bit identifier (`id & 0xFF`, `(id >> 8) & 0xFF`, `(id >> 16) & 0xFF`, `id >> 24`), as J1939-71 defines it. See `j1939_tsc1_checksum()` |

Identifier example: priority 3, from `0x27` to `0x00` → `0x0C000027`.

### EEC1, Electronic Engine Controller 1 (PGN 61444, broadcast), every 10 ms

| Byte | Content | Scaling |
|---|---|---|
| 0, bits 0-3 | Engine torque mode (SPN 899) | `0` while the engine's own governor is in control, `0xF` (not available) otherwise |
| 1 | Driver's demand engine percent torque (SPN 512) | 1 %/bit, offset −125 % |
| 2 | Actual engine percent torque (SPN 513) | 1 %/bit, offset −125 % |
| 3-4 | Engine speed (SPN 190), little-endian | 0.125 rpm/bit |
| 5 | Source address of the controlling device (SPN 1483) | your address while your TSC1 is honoured; `0x00` otherwise |

Percent torque is relative to the engine reference torque, 1600 Nm for `d936la6`.

### ET1, Engine Temperature 1 (PGN 65262), every 1000 ms

| Byte | Content | Scaling |
|---|---|---|
| 0 | Engine coolant temperature (SPN 110) | 1 °C/bit, offset −40 °C |

### EFL/P1, Engine Fluid Level/Pressure 1 (PGN 65263), every 500 ms

| Byte | Content | Scaling |
|---|---|---|
| 3 | Engine oil pressure (SPN 100) | 4 kPa/bit |

### DM1, active diagnostic trouble codes (PGN 65226, broadcast from the engine), every 1000 ms and at once on a change (since v0.3.1)

| Byte | Content | Values |
|---|---|---|
| 0 | Lamps, 2 bits each: MIL (bits 7-6), red stop (5-4), amber warning (3-2), protect (1-0) | `00` off, `01` on |
| 1 | Lamp flash status | `0xFF` |
| 2 + 4k … 5 + 4k | DTC k: SPN bits 0-7; SPN bits 8-15; SPN bits 16-18 in bits 7-5 and FMI in bits 4-0; conversion method in bit 7 (0) and occurrence count in bits 6-0 | no active code: one DTC of all zeros |

The simulated engine reports two codes:
- `fault.engine_dtc_red`: SPN 100 (oil pressure) FMI 1, with the red stop lamp. A controller must release, as for any fault.
- `fault.engine_dtc_amber`: SPN 110 (coolant temperature) FMI 16, with the amber lamp. A controller should limp home.

With one code or none, DM1 is one 8-byte frame, padded with `0xFF`. With two, it is 10 bytes and travels as a **BAM** (J1939-21):
- a TP.CM to global, `0x18ECFF00`: control byte 32, size, number of packets, `0xFF`, PGN `0x00FECA` low byte first;
- then TP.DT frames, `0x18EBFF00`, 50 ms apart, each with its sequence number and 7 bytes, the last one padded with `0xFF`.

The reference controller drops a BAM whose packet is out of sequence or later than 750 ms (T1). The engine itself keeps running: the codes are reports, not shutdowns.

### OC_OPERATOR, operator controls (open-crank, PGN 65312 / `0xFF20`, from `0xFC`), every 20 ms

**Not part of J1939.** Proprietary B range. Stands in for a CAN joystick or operator panel.

| Byte | Content | Scaling |
|---|---|---|
| 0-1 | Target engine speed (hand throttle), little-endian | 0.125 rpm/bit; `0xFFFF` = not set |
| 2 | Hoist lever | 0.4 %/bit (0–100 %) |
| 3, bits 0-1 | Fault-reset button | `00` released, `01` pressed |

### OC_PUMP_CMD, hoist pump displacement (open-crank, PGN 65313 / `0xFF21`, from the controller), every 10 ms

**Not part of J1939.** Stands in for the pump's proportional-solenoid output.

| Byte | Content | Scaling |
|---|---|---|
| 0 | Displacement command | 0.4 %/bit (0–100 %) |

## How the simulated engine behaves

- **Starting** is the operator's job (scenario signals `key_on`, `start`); the engine ECU cranks, fuels above 100 rpm and hands over to its governor above 500 rpm.
- **Speed control.** A TSC1 in speed-control mode with a valid requested speed moves the governor setpoint, clamped to 750–2100 rpm. The engine then reports your address in EEC1 byte 5.
- **Other override modes** (torque control, limits) are treated as "override disabled" in v0.1: the engine returns to low idle.
- **Stale requests.** If no TSC1 arrives for 200 ms the engine drops the request and returns to low idle on its own governor.
- **Integrity (since v0.3.1).** The engine refuses a TSC1 whose checksum (SPN 4207) is wrong, or whose counter (SPN 4206) is the same as that of the last TSC1 it accepted. A refused TSC1 counts as a lost one. A counter that jumps (frames lost on the way) is accepted. A device under test written before v0.3.1 sends byte 7 as `0xFF`: its counter never moves, so the engine refuses every TSC1 after the first. It never gets speed control and fails criterion G3.
- **Overspeed.** Above 2400 rpm the engine cuts fuel and stays tripped until the shaft has stopped. It does not restart by itself.
- **Pump.** A displacement command older than 200 ms counts as zero.

## What a controller is expected to do

The reference controller does the following, and the closed-loop tests check it. Your controller may do more, and it does not have to do it this way.

- Wait for the engine: a valid EEC1 with speed above 600 rpm for 500 ms.
- Ask for the operator's target speed with a slew limit, through TSC1 every 10 ms.
- Check that it is actually in control: EEC1 byte 5 must show its address within 500 ms.
- On loss of EEC1 (200 ms), overspeed (2300 rpm), stall (below 400 rpm) or a rejected override: release the override (TSC1 mode `00`), command zero displacement, and stay in FAULT until the operator presses fault reset.

This section describes the reference controller. What any controller must do to pass the acceptance suite, judged only by the frames it sends, is in [acceptance.md](acceptance.md).

## Trying your controller

```sh
./build/open-crank-sil vcan0 scenarios/hoist_step.csv   # the simulated machine
your-controller vcan0                                   # your device under test
candump vcan0
```

To exercise faults, run the other files in `scenarios/` (signals starting with `fault.`), or write your own; see [ARCHITECTURE.md](../ARCHITECTURE.md#46-runtime-simruntime).

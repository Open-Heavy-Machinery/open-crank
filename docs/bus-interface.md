# Bus interface (Level 1)

This is the contract between the simulated machine and a **Level 1 device under test**: a machine controller that asks the engine for speed and reads its status. If you write your own controller, this page is all it needs to know. The reference implementation is [`portable/machine_ctrl`](../portable/machine_ctrl).

Everything travels as 29-bit CAN frames with J1939 framing, so the same controller works against the SIL on `vcan`, in an emulator, and (from v0.2) on a real board.

> **Verify against your copy of SAE J1939-71.** The J1939 layouts below are original work from public sources. The repository ships no spec text.

## Nodes and addresses

| Node | Source address | Sends | Notes |
|---|---|---|---|
| Engine (simulated engine ECU) | `0x00` | EEC1, ET1, EFL/P1 | Engine #1. Silent while the key is off. |
| Machine controller (device under test) | `0x27` | TSC1 (to `0x00`), OC_PUMP_CMD | Default of the reference controller; the engine accepts TSC1 from any address. |
| Bench (operator panel, environment) | `0xFC` | OC_OPERATOR | Always on. |

## Messages

Byte numbers are **0-based** (J1939 documents count from 1). Unused bytes and bits are `0xFF` / `1`. One-byte values `0xFB`–`0xFF` and two-byte values above `0xFAFF` mean "error" or "not available" and must not be read as numbers.

### TSC1, Torque/Speed Control 1 (PGN 0, to the engine), every 10 ms while overriding

| Byte | Content | Scaling |
|---|---|---|
| 0, bits 0-1 | Override control mode (SPN 695): `00` disabled, `01` speed control, `10` torque control, `11` speed/torque limit | |
| 0, bits 2-3 | Requested speed control conditions (SPN 696) | ignored by the simulated engine |
| 0, bits 4-5 | Override control mode priority (SPN 897), `00` highest .. `11` lowest | ignored by the simulated engine |
| 1-2 | Requested speed (SPN 898), little-endian | 0.125 rpm/bit |
| 3 | Requested torque (SPN 518) | 1 %/bit, offset −125 % |
| 7 | Message counter and checksum (SPN 4206/4207) | `0xFF` in v0.1: not available |

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
- **Overspeed.** Above 2400 rpm the engine cuts fuel and stays tripped until the shaft has stopped. It does not restart by itself.
- **Pump.** A displacement command older than 200 ms counts as zero.

## What a controller is expected to do

The reference controller does the following, and the closed-loop tests check it. Your controller may do more, and it does not have to do it this way.

- Wait for the engine: a valid EEC1 with speed above 600 rpm for 500 ms.
- Ask for the operator's target speed with a slew limit, through TSC1 every 10 ms.
- Check that it is actually in control: EEC1 byte 5 must show its address within 500 ms.
- On loss of EEC1 (200 ms), overspeed (2300 rpm), stall (below 400 rpm) or a rejected override: release the override (TSC1 mode `00`), command zero displacement, and stay in FAULT until the operator presses fault reset.

## Trying your controller

```sh
./build/open-crank-sil vcan0 scenarios/hoist_step.csv   # the simulated machine
your-controller vcan0                                   # your device under test
candump vcan0
```

To exercise faults, run the other files in `scenarios/` (signals starting with `fault.`), or write your own; see [ARCHITECTURE.md](../ARCHITECTURE.md#46-runtime-simruntime).

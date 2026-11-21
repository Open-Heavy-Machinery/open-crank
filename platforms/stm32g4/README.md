# STM32G4 demo (v0.2, step 1)

## What this is

The first step of [v0.2](../../ROADMAP.md#v02-reference-board-on-the-bus): an STM32G431KB running the reference machine controller (`portable/machine_ctrl`), as a Level 1 device under test, against `open-crank-sil` over a real CAN bus.

It is a demo to paste into an STM32CubeIDE project, not a build of its own: [`demo/main_user_code.c`](demo/main_user_code.c) holds only the `USER CODE` zones of the project's `main.c`.

## Hardware

- STM32G431KB board.
- A CAN transceiver.
- A USB-CAN adapter (CANable).
- 120 Ω termination at both ends of the bus.

There is no bill of materials yet; it comes in a later step of v0.2.

## STM32CubeIDE project

Set these in the `.ioc` and let CubeIDE generate the code; do not edit `MX_FDCAN1_Init` by hand, or the next generation overwrites it.

**FDCAN1**

| Setting | Value |
|---|---|
| Frame format | Classic mode (`FDCAN_FRAME_CLASSIC`) |
| Mode | Normal |
| Auto retransmission | Enabled |
| Nominal prescaler | 17 |
| Nominal sync jump width | 5 |
| Nominal time seg 1 | 34 |
| Nominal time seg 2 | 5 |
| Bit rate | 250 kbit/s (170 MHz / 17 / (1 + 34 + 5)) |
| Sample point | 87.5 % ((1 + 34) / 40) |
| Identifiers | extended (29-bit) |
| TX | FIFO (`FDCAN_TX_FIFO_OPERATION`) |

**Clock.** HSI (16 MHz) into the PLL with M = 4, N = 85, R = 2: 170 MHz system clock; AHB, APB1 and APB2 at 170 MHz; FDCAN kernel clock 170 MHz. Regulator in Scale 1 boost mode, flash latency 4. The board's 24 MHz HSE crystal is not used: enabling it needs a hardware change on the Nucleo (see ST's UM2397). The HSI is less accurate than a crystal; that is enough for this two-node bench, not for a vehicle network.

**Pins**

| Pin | Signal |
|---|---|
| PA11 | FDCAN1_RX |
| PA12 | FDCAN1_TX |
| PA2 | USART2_TX |
| PA3 | USART2_RX |
| PB8 | GPIO output, `LED_USER` |

## Bringing in the code without copying files

1. **Link `portable/`** into the project as a linked folder: *New > Folder > Advanced > Link to alternate location*, pointing at this repository's `portable/`.
2. **Add include paths** (*Properties > C/C++ Build > Settings > MCU GCC Compiler > Include paths*), as **absolute paths**:
   - `<path to open-crank>/portable/j1939/include`
   - `<path to open-crank>/portable/machine_ctrl/include`

   A build variable such as `${...}` does not resolve in the maintainer's installation; see the notes.
3. **Paste [`demo/main_user_code.c`](demo/main_user_code.c)** into the matching `USER CODE` zones of `Core/Src/main.c`: `Includes`, `PV`, `0`, `2` and `WHILE`. The `WHILE` zone opens the `while (1)` loop; its closing brace is the generated one after `USER CODE BEGIN 3`.

## Running the demo

```sh
sudo slcand -o -c -s5 /dev/ttyACM0 can0
sudo ip link set can0 up
./build/open-crank-sil can0 scenarios/hoist_step.csv
candump can0
```

`-s5` is 250 kbit/s. (`-s6` is 500 kbit/s, which is not the J1939 rate used here.)

Reset the board before each run. When `open-crank-sil` stops, the controller loses EEC1 and latches `MC_FAULT_ENGINE_STATUS_LOST`; it leaves the fault only on a fault reset, which `hoist_step.csv` does not send. Without a reset, the next run shows `speed set by 0x00` and the pump at 0 %: the controller is holding its safe state.

The Nucleo's ST-LINK also shows up as `/dev/ttyACM*`, and the numbers can swap after a USB reconnect. Give `slcand` the adapter's path under `/dev/serial/by-id/` instead of `/dev/ttyACM0`.

## What you should see

- In `candump`, frames with identifier `0x0C000027` (TSC1 from the controller) every 10 ms.
- In `open-crank-sil`, `speed set by 0x27`.
- The engine speed following the operator's requests.

## Status

Tested on the board: yes, 2026-11-21, one run of `hoist_step` against `open-crank-sil` on `can0`: `speed set by 0x27` from 2.0 s, rpm steady at 1500, pump at 85 % during the step.

Bit timing changed to a sample point of 87.5 % (2026-11-21) and `hoist_step` re-run on the board with it: every run started after a reset gave the same result; a run started without a reset showed the latched fault described in "Running the demo".

This is not yet the comparison against the SIL paced by the wall clock that v0.2's exit criteria ask for.

## Notes

- On the G4, the HAL's `DataLength` is the number of bytes. On the H7 it is shifted left by 16 bits.
- The absolute include paths are temporary. A reproducible build (CMake) is still to be decided.

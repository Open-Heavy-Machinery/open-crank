# STM32G4 demo (v0.2, step 1)

## What this is

The first step of [v0.2](../../ROADMAP.md#v02-reference-board-on-the-bus): an STM32G431KB running the reference machine controller (`portable/machine_ctrl`), as a Level 1 device under test, against `open-crank-sil` over a real CAN bus.

It is a demo to paste into an STM32CubeIDE project, not a build of its own: [`demo/main_user_code.c`](demo/main_user_code.c) holds only the `USER CODE` zones of the project's `main.c`. [`echo/main_user_code.c`](echo/main_user_code.c) is the same kind of file for the round-trip measurement below.

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
sudo slcand -o -c -s5 /dev/serial/by-id/<your-adapter> can0
sudo ip link set can0 up
./build/open-crank-sil can0 scenarios/hoist_step.csv
candump can0
```

`-s5` is 250 kbit/s. (`-s6` is 500 kbit/s, which is not the J1939 rate used here.)

Reset the board before each run. When `open-crank-sil` stops, the controller loses EEC1 and latches `MC_FAULT_ENGINE_STATUS_LOST`; it leaves the fault only on a fault reset, which `hoist_step.csv` does not send. Without a reset, the next run shows `speed set by 0x00` and the pump at 0 %: the controller is holding its safe state.

The Nucleo's ST-LINK also shows up as `/dev/ttyACM*`, and the numbers can swap after a USB reconnect, so the commands use the adapter's path under `/dev/serial/by-id/`; `ls -l /dev/serial/by-id/` shows which one it is.

## What you should see

- In `candump`, frames with identifier `0x0C000027` (TSC1 from the controller) every 10 ms.
- In `open-crank-sil`, `speed set by 0x27`.
- The engine speed following the operator's requests.

## Measuring the USB-CAN round trip

[`echo/main_user_code.c`](echo/main_user_code.c) is for a second, separate CubeIDE project: the board answers each request `0x18FF2EFC` with `0x18FF2F27` and the same 8 bytes, from the FDCAN RX FIFO 0 interrupt, and does nothing else. Make it from a copy of the demo project without the demo code and without `portable/`, and enable **FDCAN1 interrupt 0** in the `.ioc` (FDCAN1 → NVIC Settings). With `open-crank-sil` stopped, on the PC:

```sh
tools/can_rtt.py can0 --csv rtt_can0.csv
```

Measured on 2026-11-22: NUCLEO-G431KB with the settings above, CANable over slcan at 250 kbit/s, one run of 5000 requests at 100 Hz, with a `candump` running at the same time. 5000 echoed, 0 lost; on the board `echo_count` 5000 and `echo_dropped` 0. Round trip: min 1.285 ms, p50 1.660 ms, p99 1.949 ms, max 4.170 ms. It is the round trip seen by a program on the PC, both directions, not halved. Roughly 1.1 ms of it is the two frames on the wire at 250 kbit/s (an estimate from the frame length, not a measurement).

## Execution time of `mc_step`

The demo times every `mc_step` call with the DWT cycle counter and keeps the largest in `mc_step_cycles_max`; read it in a CubeIDE debug session (for example in Live Expressions). It is the worst observed execution time, not a proven bound.

Measured on 2026-11-22 at 170 MHz with optimisation `-O0` (CubeIDE's Debug default), one run per scenario, each after a board reset:

| Scenario | Cycles | µs |
|---|---|---|
| `cold_start` | 2224 | 13.1 |
| `engine_status_loss` | 2248 | 13.2 |
| `hoist_step` | 2230 | 13.1 |
| `override_ignored` | 2287 | 13.5 |
| `overspeed` | 2224 | 13.1 |
| `speed_command_loss` | 2286 | 13.4 |

Worst observed: 2287 cycles (13.5 µs), about 0.13 % of the 10 ms period. Another optimisation level gives other figures.

## Stack usage of the controller

`tools/check_portable.sh` builds `portable/` for the Cortex-M4F (MinSizeRel) with `-fcallgraph-info=su`, and `tools/stack_budget.py` adds up the stack along the worst call path from each public function of the controller. CI fails on recursion, on an indirect call, or on a path above 1024 bytes. As printed by the CI job `portable/ on Cortex-M4F`, with the CI's `arm-none-eabi-gcc` (`15:14.2.rel1-1`):

| Function | Worst-case stack |
|---|---|
| `mc_init` | 16 bytes |
| `mc_handle_frame` | 96 bytes |
| `mc_step` | 68 bytes |
| `mc_clear_fault` | 0 bytes |

`memcpy`, `memmove`, `memset` and `memcmp` are not counted. This is the controller's stack only: the demo's own code, the HAL and the interrupt handlers come on top, and the demo's build (CubeIDE, `-O0`) is not the build measured here. Other GCC versions give other figures (GCC 10.3.1: `mc_step` 76 bytes).

## Status

Tested on the board: yes, 2026-11-21, several runs of `hoist_step` against `open-crank-sil` on `can0` with the settings above (sample point 87.5 %, SJW 5), each started after a reset: `speed set by 0x27` from 1.5 s, rpm steady at 1500, pump at 85 % during the step. A run started without a reset showed the latched fault described in "Running the demo".

The comparison against the SIL paced by the wall clock passed on 2026-11-22 (three board runs); see [docs/board-vs-sil.md](../../docs/board-vs-sil.md#results).

## Notes

- On the G4, the HAL's `DataLength` is the number of bytes. On the H7 it is shifted left by 16 bits.
- The absolute include paths are temporary. A reproducible build (CMake) is still to be decided.

# STM32G4 reference board (v0.2)

## What this is

The reference board of [v0.2](../../ROADMAP.md#v02-reference-board-on-the-bus): an STM32G431KB running the reference machine controller (`portable/machine_ctrl`), as a Level 1 device under test, against `open-crank-sil` over a real CAN bus. To set it up from nothing, follow [BRINGUP.md](BRINGUP.md).

There are two ways to build it:

- **With CMake, from this repository alone** (v0.2.2): [`demo/main.c`](demo/main.c) and [`echo/main.c`](echo/main.c) are complete firmware, with the board set-up in [`board/`](board/). This is the reference for tests and automation: see [Build and program without CubeIDE](#build-and-program-without-cubeide).
- **With STM32CubeIDE**, the entry point for beginners and for applications that depend on CubeIDE: [`demo/main_user_code.c`](demo/main_user_code.c) holds only the `USER CODE` zones of a CubeIDE project's `main.c`, and [`echo/main_user_code.c`](echo/main_user_code.c) the same for the round-trip echo. Follow [BRINGUP.md](BRINGUP.md). Test automation does not use this path.

Both have the same behaviour; when one changes, the other must follow.

## Hardware

**Bill of materials** (the maintainer's bench)

| Qty | Part | Notes |
|---|---|---|
| 1 | NUCLEO-G431KB | STM32G431KB; powered and programmed through its micro-USB (ST-LINK) |
| 1 | MCP2562FD CAN transceiver, DIP-8 | on a breadboard; VDD from the Nucleo's 5V, VIO from its 3V3 |
| 1 | USB-CAN adapter compatible with CANable 2.0 (slcan firmware), USB-C | its on-board 120 Ω termination enabled with its jumper |
| 1 | 120 Ω resistor, ±5 % | between CANH and CANL at the transceiver: the other end of the bus |
| 1 | breadboard and jumper wires | |
| 1 | micro-USB cable | for the Nucleo |
| 1 | USB-C cable | for the adapter (USB-C to USB-C or to USB-A, as the PC needs) |
| 2 | 100 nF ceramic capacitors | **recommended, not fitted on the maintainer's bench**: decoupling at the transceiver's VDD and VIO pins, as its datasheet recommends |

**Wiring**

| MCP2562FD pin | Connects to |
|---|---|
| 1 TXD | PA12 (FDCAN1_TX) |
| 2 VSS | GND |
| 3 VDD | 5V of the Nucleo |
| 4 RXD | PA11 (FDCAN1_RX) |
| 5 VIO | 3V3 of the Nucleo |
| 6 CANL | CANL of the adapter |
| 7 CANH | CANH of the adapter |
| 8 STBY | GND (normal mode) |

The 120 Ω resistor goes between pins 6 and 7; with the adapter's termination on, the bus has one terminator at each end (about 60 Ω between CANH and CANL with everything unpowered). VIO at 3.3 V keeps TXD and RXD at the STM32's logic level while the bus side runs from 5 V. Pin numbers are those of Microchip's MCP2562FD datasheet; check them against your copy.

## Build and program without CubeIDE

On a Linux PC (Debian or Ubuntu), with the Nucleo on USB:

```sh
tools/board/setup.sh          # checks the PC; installs what is missing after asking
tools/board/flash.sh demo     # builds, programs, verifies and starts the reference controller
tools/board/reset.sh          # resets the board, as the reset button does
```

- `setup.sh` installs `openocd`, `can-utils`, `cmake` and a host compiler with `apt` (it asks first), and the pinned Arm GNU Toolchain 14.2.rel1 into `~/.cache/open-crank` without `sudo` ([`tools/board/arm_toolchain.sh`](../../tools/board/arm_toolchain.sh), SHA-256 checked). It reports, and never changes, group membership (`plugdev`, `dialout`) and whether an ST-LINK and a USB-CAN adapter are plugged in. `--check` installs nothing.
- `flash.sh echo` programs the round-trip echo instead. `--no-build` programs what is already built.
- Programming and reset go through the Nucleo's on-board ST-LINK with OpenOCD (`interface/stlink.cfg`, `target/stm32g4x.cfg`); the reset is a system reset over SWD, no wire to add.

Or by hand, with the pinned toolchain first on `PATH`:

```sh
cmake -S platforms/stm32g4 -B build-stm32g4 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/arm-none-eabi.cmake
cmake --build build-stm32g4
openocd -f interface/stlink.cfg -f target/stm32g4x.cfg -c "program build-stm32g4/oc-stm32g4-demo.elf verify reset exit"
```

The build type defaults to Debug (`-O0 -g`), as CubeIDE's, which is how the execution times below were measured. CI builds both firmwares on every push (job `firmware`) and keeps the ELF, BIN and MAP files as an artefact.

### The bench check

`tools/board/bench.sh [--iface can0] [--include-serials] [SCENARIO...]` is the standard check of a physical setup. Run it before bringing your own firmware, and after changing anything on the bench. The report names the ST-LINK and the USB-CAN adapter by their `/dev/serial/by-id` names. Their serial numbers are written as `<serial>`, so a report can be shared as it is; `--include-serials` keeps them, for example to tell two benches with the same hardware apart.

1. **Round trip.** It programs `oc-stm32g4-echo` and sends 1000 requests at 100 Hz (`tools/can_rtt.py`). The limits are 0 lost and p99 ≤ 5 ms, half a controller step: above that, the bus could move reactions by more than one step. On the reference bench (2026-11-24): p50 1.499 ms, p99 1.735 ms.
2. **The reference controller.** It programs `oc-stm32g4-demo`. OpenOCD's verify makes sure that the board runs exactly the firmware built from this commit. Then, for every scenario:
   - it resets the board;
   - it waits up to 2 s for its first TSC1. A fresh reset starts the controller in WAIT_ENGINE, never in FAULT, and the frame shows it is alive;
   - it runs `open-crank-accept`.
3. **Report.** `report.md` in the run directory (`bench-<date>-<time>/`), next to each scenario's report, trace and verdict file (`<scenario>.verdicts.csv`, open-tick's verdict format), and the round-trip CSV. Open a trace with its verdict file in [open-rig](https://github.com/Open-Heavy-Machinery/open-rig) to see where each criterion's window, reaction and any failure fall on the signals. Every reaction is compared with the reference bench ([`tools/board/bench_reference.csv`](../../tools/board/bench_reference.csv), the v0.2.1 board run). A reaction matches when it is within one 10 ms step of the reference: it lands in the same step or the next one, depending on where the board's tick falls after the reset.

Exit 0 **BENCH OK**, 1 the bench does not match (the report lists why), 2 the PC or the CAN interface is not ready. A full run takes about 4 minutes with the eleven scenarios (about 2.5 minutes with the seven before v0.3). On the reference bench, on `18060be` (2026-11-25, eleven scenarios): BENCH OK; round trip 0 lost, p99 1.792 ms; eleven PASS, 0 late steps, every reaction within one step of v0.2.1's (the link-fault scenarios are the reference themselves). Earlier, on v0.2.2's commit (2026-11-24): BENCH OK; round trip 0 lost, p99 1.895 ms; seven PASS, 0 late steps, every reaction within one step.

### Where each piece comes from

| Piece | Source | Licence |
|---|---|---|
| `board/` (clock, FDCAN1, HAL configuration, linker script), `demo/main.c`, `echo/main.c` | written in this repository | Apache-2.0 |
| `portable/` (the controller) | this repository, unchanged | Apache-2.0 |
| CMSIS Core (`core_cm4.h` and friends) | [STMicroelectronics/cmsis_core](https://github.com/STMicroelectronics/cmsis_core) `v5.9.0_20250520`, downloaded at configure time | Apache-2.0 |
| Device header, `system_stm32g4xx.c`, `startup_stm32g431xx.s` | [STMicroelectronics/cmsis_device_g4](https://github.com/STMicroelectronics/cmsis_device_g4) `v1.2.6`, downloaded, used as they come | Apache-2.0 |
| HAL (RCC, PWR, FLASH, GPIO, CORTEX, FDCAN) | [STMicroelectronics/stm32g4xx_hal_driver](https://github.com/STMicroelectronics/stm32g4xx_hal_driver) `v1.2.7`, downloaded, used as it comes | BSD-3-Clause |

Each download is pinned by its SHA-256 in [`CMakeLists.txt`](CMakeLists.txt). Nothing generated by STM32CubeMX and no file from ST is in this repository.

## STM32CubeIDE project

Set these in the `.ioc` and let CubeIDE generate the code; do not edit `MX_FDCAN1_Init` by hand, or the next generation overwrites it.

**FDCAN1**

Only the *Nominal* fields count in Classic mode; leave the *Data* fields as CubeMX sets them. The sync jump width goes in *Nominal Sync Jump Width*.

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

| Pin | Signal | Used by the demo |
|---|---|---|
| PA11 | FDCAN1_RX | yes |
| PA12 | FDCAN1_TX | yes |
| PA2 | USART2_TX | no: board default, can stay or go |
| PA3 | USART2_RX | no: board default, can stay or go |
| PB8 | GPIO output, `LED_USER` | no: board default, can stay or go |

## Bringing in the code without copying files

1. **Link `portable/`** into the project as a linked folder: *New > Folder > Advanced > Link to alternate location*, pointing at this repository's `portable/`.
2. **Add `portable` as a source location** (*Project > Properties > C/C++ General > Paths and Symbols > Source Location > Add Folder…*, then select `portable`). A generated project compiles only `Core` and `Drivers`; without this step the link fails with undefined references to `mc_init` and the other controller functions.
3. **Add include paths** (*Project > Properties > C/C++ Build > Settings > MCU GCC Compiler > Include paths*), as **absolute paths**:
   - `<path to open-crank>/portable/j1939/include`
   - `<path to open-crank>/portable/machine_ctrl/include`

   A build variable such as `${...}` does not resolve in the maintainer's installation; see the notes.
4. **Paste [`demo/main_user_code.c`](demo/main_user_code.c)** into the matching `USER CODE` zones of `Core/Src/main.c`: `Includes`, `PV`, `0`, `2` and `WHILE`. The `WHILE` zone opens the `while (1)` loop; its closing brace is the generated one after `USER CODE BEGIN 3`.

## Running the demo

```sh
sudo slcand -o -c -s5 /dev/serial/by-id/<your-adapter> can0
sudo ip link set can0 up
./build/open-crank-sil can0 scenarios/hoist_step.csv
candump can0
```

`-s5` is 250 kbit/s. (`-s6` is 500 kbit/s, which is not the J1939 rate used here.)

Reset the board before each run (`tools/board/reset.sh`, or the button). When `open-crank-sil` stops, the controller loses EEC1 and latches `MC_FAULT_ENGINE_STATUS_LOST`; it leaves the fault only on a fault reset, which `hoist_step.csv` does not send. Without a reset, the next run shows `speed set by 0x00` and the pump at 0 %: the controller is holding its safe state.

The Nucleo's ST-LINK also shows up as `/dev/ttyACM*`, and the numbers can swap after a USB reconnect, so the commands use the adapter's path under `/dev/serial/by-id/`; `ls -l /dev/serial/by-id/` shows which one it is.

## What you should see

- In `candump`, frames with identifier `0x0C000027` (TSC1 from the controller) every 10 ms: first data byte `F0` (override disabled) until the controller takes control, then `F1`.
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
| `mc_init_after_watchdog` | 24 bytes |
| `mc_handle_frame` | 96 bytes |
| `mc_step` | 76 bytes |
| `mc_clear_fault` | 0 bytes |

`memcpy`, `memmove`, `memset` and `memcmp` are not counted. This is the controller's stack only: the demo's own code, the HAL and the interrupt handlers come on top, and the demo's build (CubeIDE, `-O0`) is not the build measured here. Other GCC versions give other figures (GCC 10.3.1: `mc_step` 84 bytes). `mc_step` grew from 68 to 76 bytes in v0.3.1 with the TSC1 counter and checksum.

### The watchdog check

The reference firmware runs the independent watchdog (`board/watchdog.c`, IWDG, 100 ms, register level, no HAL module), kicked once per 10 ms control period. At start-up it reads the reset cause and then clears the reset flags (`RCC->CSR |= RCC_CSR_RMVF`), so that a later reset of another kind is not taken for a watchdog one. After a watchdog reset the controller starts in FAULT (`mc_init_after_watchdog`, docs/safety.md SR-11); after any other reset, in WAIT_ENGINE. While a debugger halts the core, the watchdog stops (`DBG_IWDG_STOP`).

`tools/board/watchdog_check.sh [--iface can0] [--out DIR]` tests it on the bus, in about 30 s:
- it programs `oc-stm32g4-demo-hangtest`. This build exists only for the test: it is the demo compiled with `OC_WDG_HANG_TEST`, which stops the control loop when the bench sends the test frame `0x18FF2DFC` (signal `test.dut_hang`). CI fails if the reference firmware contains the hang (`oc_test_hang`);
- it plays `scenarios/board/watchdog_reset.csv`;
- whatever happens, it programs the reference controller back.

On the reference bench (2026-11-25): PASS. W1 reacts in 120 ms, before the 200 ms after which silence alone would count as a release, so the board did restart and send mode `00` from FAULT. W2 takes control again 530 ms after the fault reset.

The watchdog is in the CMake build only; `demo/main_user_code.c` (CubeIDE) does not have it yet.

## Status

Tested on the board: yes, 2026-11-21, several runs of `hoist_step` against `open-crank-sil` on `can0` with the settings above (sample point 87.5 %, SJW 5), each started after a reset: `speed set by 0x27` from 1.5 s, rpm steady at 1500, pump at 85 % during the step. A run started without a reset showed the latched fault described in "Running the demo".

The comparison against the SIL paced by the wall clock passed on 2026-11-22 (three board runs); see [docs/board-vs-sil.md](../../docs/board-vs-sil.md#results).

## Notes

- On the G4, the HAL's `DataLength` is the number of bytes. On the H7 it is shifted left by 16 bits.
- The absolute include paths are temporary. A reproducible build (CMake) is still to be decided.

# Bring-up from scratch: the STM32G4 reference bench

> **Two ways in.** This guide builds the demo with STM32CubeIDE, step by step. Start here if you are new to STM32, or if your own application depends on CubeIDE. Test automation does not use this path. Since v0.2.2 the firmware also builds with CMake, and [`tools/board/bench.sh`](../../tools/board/bench.sh) programs and tests the board without CubeIDE (see [README.md](README.md#build-and-program-without-cubeide)). The hardware and wiring are the same for both.

This guide goes from nothing to the reference controller on a NUCLEO-G431KB, controlling the simulated engine over a real CAN bus. It starts from a clean clone and links to the [STM32G4 README](README.md) for the tables (bill of materials, wiring, FDCAN and clock settings) instead of copying them. Exit criterion 2 of [v0.2](../../ROADMAP.md#v02-reference-board-on-the-bus) asks for it to have been followed from scratch; see the record at the end.

## 0. What you need

- **Hardware:** the [bill of materials](README.md#hardware).
- **A Linux PC** with SocketCAN and the `slcan` kernel module (most desktop distributions have both), and:
  - `git`, CMake 3.20 or newer, a C11 compiler and `python3`;
  - `can-utils` (`slcand`, `candump`, `cansend`); on Debian or Ubuntu: `sudo apt install can-utils`.
- **STM32CubeMX and STM32CubeIDE**, from ST. With STM32CubeIDE 2.x the project is generated in STM32CubeMX, which is a separate application.

## 1. Clone and build the PC side

```sh
git clone --recursive https://github.com/Open-Heavy-Machinery/open-crank.git
cd open-crank
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

All tests must pass. Remember the absolute path of this clone: the CubeIDE project uses it in step 5.

## 2. Wire the bench

Wire it as in [Wiring](README.md#hardware), with everything unpowered. Enable the adapter's 120 Ω termination. Before powering anything, measure the resistance between CANH and CANL: about 60 Ω means one terminator at each end. About 120 Ω means one is missing; a few ohms or less means a short.

## 3. Bring up the adapter

Plug the adapter in, then find its path (the Nucleo's ST-LINK also appears as a `ttyACM`):

```sh
ls -l /dev/serial/by-id/
sudo slcand -o -c -s5 /dev/serial/by-id/<your-adapter> can0
sudo ip link set can0 up
```

`-s5` is 250 kbit/s. `ip -details link show can0` should show the interface `UP`.

## 4. Generate the project in STM32CubeMX

1. **New project for the board.** In STM32CubeMX, start a new project from the board selector with the NUCLEO-G431KB, and accept the board's default peripheral setup. The defaults also enable USART2 and the user LED; the demo uses neither, so you can leave them or remove them.
2. **FDCAN1.** Activate FDCAN1 and check that its pins are PA11 (RX) and PA12 (TX); if CubeMX placed them elsewhere, move them. These are the only pins the demo needs.
3. **FDCAN1 parameters.** Set the values of the [FDCAN1 table](README.md#stm32cubeide-project). Only the *Nominal* fields count in Classic mode: the sync jump width goes in *Nominal Sync Jump Width*. Leave the *Data* fields as CubeMX sets them. A value typed into *Data Sync Jump Width* by mistake still gives 250 kbit/s, but with the wrong sync jump width.
4. **Clock.** Set the clock tree as in the [Clock paragraph](README.md#stm32cubeide-project): HSI into the PLL, 170 MHz system clock, FDCAN kernel clock from PCLK1 at 170 MHz.
5. **Project Manager.** Set *Toolchain / IDE* to **STM32CubeIDE**, give the project a name and a location, and generate the code. Then open the generated project in STM32CubeIDE.
6. **From now on** change settings only in the `.ioc`, never by editing `MX_FDCAN1_Init` by hand: the next generation would overwrite it.

## 5. Bring in the controller

Follow [Bringing in the code without copying files](README.md#bringing-in-the-code-without-copying-files): link `portable/` as a folder, add it as a source location, add the two include paths as absolute paths to your clone, and paste [`demo/main_user_code.c`](demo/main_user_code.c) into the matching `USER CODE` zones of `Core/Src/main.c`. Without the source location, the headers are found but the controller's `.c` files are not compiled, and the link fails with undefined references to `mc_init`, `mc_step`, `mc_handle_frame` and `MC_DEFAULT_PARAMS`.

## 6. Build and flash

Build the project, connect the Nucleo by its micro-USB and run it on the board (*Run As > STM32 C/C++ Application*, through the on-board ST-LINK). A debug session works too, and is how you read `mc_step_cycles_max` ([Execution time](README.md#execution-time-of-mc_step)).

## 7. First check: the board on the bus

With the board running and no simulator yet:

```sh
candump can0
```

The board sends TSC1 to the engine every 10 ms: identifier `0C000027`, first data byte `F0` (override disabled) because no engine is running yet. Once the engine runs and the board takes control, that byte becomes `F1` (speed control). If nothing appears, see step 9.

## 8. Run `hoist_step` against the simulator

Reset the board, then in the clone:

```sh
./build/open-crank-sil can0 scenarios/hoist_step.csv
```

Expect `speed set by 0x27` from about 1.5 s, the engine speed settling at 1500 rpm, the pump at 85.2 % from 8.5 s to 18 s, and `0 late steps` when it stops (Ctrl+C after `scenario ended`). Reset the board before every run: [why](README.md#running-the-demo).

To compare the board with the reference SIL, follow [board-vs-sil.md](../../docs/board-vs-sil.md).

## 9. If something is wrong

| What you see | Check |
|---|---|
| Link errors: undefined reference to `mc_init`, `mc_step`, `mc_handle_frame`, `MC_DEFAULT_PARAMS` | `portable` is not a source location of the project (step 5). |
| No frames at all in `candump` | Wiring and the 60 Ω reading (step 2); that `can0` is `UP` and `slcand` points at the adapter, not at the ST-LINK (step 3); that the board is running. |
| Frames only from the simulator, none `…27` from the board | FDCAN1 pins and settings in the `.ioc` (step 4); that `HAL_FDCAN_Start` runs (step 5). |
| `speed set by 0x00` from the start | The board was not reset after the previous run: it is holding its latched fault. Reset it. |
| `speed set by 0x00` in the middle of a run | A USB reconnect (`sudo dmesg`); start again from step 3. |
| Bus errors, or nothing works at a different `-s` | Both ends must be at 250 kbit/s: `-s5` on the adapter, and the FDCAN1 values in the `.ioc`. |

## Followed from scratch

Each time someone follows this guide on a clean clone, record here the date, who, the tools, and every step that did not work as written.

| Date | Who | Tools | Steps that did not work as written (all fixed above) |
|---|---|---|---|
| 2026-11-22 | the maintainer | STM32CubeIDE 2.1.1 with STM32CubeMX as a separate application; a fresh clone | 1. The project is generated in STM32CubeMX with *Toolchain / IDE* = STM32CubeIDE; the guide said *File > New > STM32 Project* in CubeIDE. 2. The link failed with undefined references until `portable` was added as a source location. 3. The Pins table listed USART2 and the LED as if the demo needed them. 4. The sync jump width went into the *Data* field instead of *Nominal*. With those fixed, the demo ran on the board against `open-crank-sil`. |

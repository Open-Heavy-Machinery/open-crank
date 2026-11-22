# Bring-up from scratch: the STM32G4 reference bench

This guide goes from nothing to the reference controller on a NUCLEO-G431KB, controlling the simulated engine over a real CAN bus. It starts from a clean clone and links to the [STM32G4 README](README.md) for the tables (bill of materials, wiring, FDCAN and clock settings) instead of copying them. Exit criterion 2 of [v0.2](../../ROADMAP.md#v02-reference-board-on-the-bus) asks for it to have been followed from scratch; see the record at the end.

## 0. What you need

- **Hardware:** the [bill of materials](README.md#hardware).
- **A Linux PC** with SocketCAN and the `slcan` kernel module (most desktop distributions have both), and:
  - `git`, CMake 3.20 or newer, a C11 compiler and `python3`;
  - `can-utils` (`slcand`, `candump`, `cansend`); on Debian or Ubuntu: `sudo apt install can-utils`.
- **STM32CubeIDE**, from ST.

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

## 4. Create the CubeIDE project

1. **New project for the board.** *File > New > STM32 Project*, select the NUCLEO-G431KB in the board selector, and give the project a name. Accept the board's default peripheral setup.
2. **FDCAN1.** In the `.ioc`, activate FDCAN1 and set every parameter of the [FDCAN1 table](README.md#stm32cubeide-project). Check that its pins are PA11 (RX) and PA12 (TX) as in the [Pins table](README.md#stm32cubeide-project); if CubeMX placed them elsewhere, move them.
3. **Clock.** Set the clock tree as in the [Clock paragraph](README.md#stm32cubeide-project): HSI into the PLL, 170 MHz system clock, FDCAN kernel clock 170 MHz.
4. **Generate the code.** Save the `.ioc` and let CubeIDE generate. From now on change settings only in the `.ioc`, never by editing `MX_FDCAN1_Init` by hand: the next generation would overwrite it.

## 5. Bring in the controller

Follow [Bringing in the code without copying files](README.md#bringing-in-the-code-without-copying-files): link `portable/` as a folder, add the two include paths as absolute paths to your clone, and paste [`demo/main_user_code.c`](demo/main_user_code.c) into the matching `USER CODE` zones of `Core/Src/main.c`.

## 6. Build and flash

Build the project, connect the Nucleo by its micro-USB and run it on the board (*Run As > STM32 C/C++ Application*, through the on-board ST-LINK). A debug session works too, and is how you read `mc_step_cycles_max` ([Execution time](README.md#execution-time-of-mc_step)).

## 7. First check: the board on the bus

With the board running and no simulator yet:

```sh
candump can0
```

The board sends TSC1 to the engine every 10 ms: identifier `0C000027`, with the override disabled because no engine is running yet. If nothing appears, see step 9.

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
| No frames at all in `candump` | Wiring and the 60 Ω reading (step 2); that `can0` is `UP` and `slcand` points at the adapter, not at the ST-LINK (step 3); that the board is running. |
| Frames only from the simulator, none `…27` from the board | FDCAN1 pins and settings in the `.ioc` (step 4); that `HAL_FDCAN_Start` runs (step 5). |
| `speed set by 0x00` from the start | The board was not reset after the previous run: it is holding its latched fault. Reset it. |
| `speed set by 0x00` in the middle of a run | A USB reconnect (`sudo dmesg`); start again from step 3. |
| Bus errors, or nothing works at a different `-s` | Both ends must be at 250 kbit/s: `-s5` on the adapter, and the FDCAN1 values in the `.ioc`. |

## Followed from scratch

Not yet *(planned)*. Each time someone follows this guide on a clean clone, record here the date, who, the STM32CubeIDE version, and every step that did not work as written.

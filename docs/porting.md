# Porting the controller to another board

The reference controller is [`portable/machine_ctrl`](../portable/machine_ctrl), with the J1939 codec in [`portable/j1939`](../portable/j1939). It is ISO C11 with no operating system, no heap and no I/O of its own. A platform runs it by doing the four things below. Two platforms in this repository do it: [`platforms/stm32g4/demo`](../platforms/stm32g4/demo/main_user_code.c) on a microcontroller, and [`platforms/linux-socketcan/mc_main.c`](../platforms/linux-socketcan/mc_main.c) on Linux.

## What the platform does

1. **Once, at start:** `mc_init(&mc, &MC_DEFAULT_PARAMS);`. `McCtx` is plain storage, one per controller.
2. **For every CAN frame received:** fill a `J1939Frame` (`id` is the 29-bit identifier with no flag bits, `dlc` is 0 to 8, `data` holds 8 bytes) and call `mc_handle_frame(&mc, &f, now_ms)`. Pass only extended (29-bit) data frames. The controller ignores the frames it does not use.
3. **Every 10 ms:** `n = mc_step(&mc, now_ms, out, cap)`, then send the `n` frames written to `out`. Today it returns at most two (TSC1 and OC_PUMP_CMD), so `cap` must be at least 2.
4. **Time:** `now_ms` is a millisecond counter that the platform keeps, as a `uint32_t` that only goes up; the controller copes with its wrap-around. Use the same counter in both calls.

## Rules

- **One context.** Call the controller's functions from the same loop or thread, never at the same time from two (for example, not `mc_handle_frame` from an interrupt while `mc_step` runs in the main loop). If frames arrive in an interrupt, queue them there and feed them from the loop. The STM32G4 demo polls the receive FIFO from its main loop.
- **Never block on send.** If the transmit queue is full, drop the frame and count it. The controller sends fresh frames every period.
- **The bus** is classical CAN, 29-bit identifiers, 250 kbit/s, J1939-21 framing: see [bus-interface.md](bus-interface.md).
- **Resources.** No heap. `float` arithmetic, so a single-precision FPU helps. From the C library it needs only `memcpy`, `memmove`, `memset` and `memcmp`. Its worst-case stack, from the call graph, is in the [STM32G4 README](../platforms/stm32g4/README.md#stack-usage-of-the-controller).
- **Bus-off** is the platform's job: the controller does not see the CAN controller. The STM32G4 demo does not recover from bus-off yet.
- **After a reset** the controller starts waiting for the engine and takes control only once the engine reports it is running.

## Checking a port

- **That `portable/` builds for your target** the way the CI checks it for the Cortex-M4F: [`tools/check_portable.sh`](../tools/check_portable.sh).
- **That it behaves the same on the bus:** run `scenarios/hoist_step.csv` with your board in the loop and compare it with the paced SIL as in [board-vs-sil.md](board-vs-sil.md).

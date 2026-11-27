# Level 2 wiring: Tang Nano 20K ↔ NUCLEO-G431KB (v0.4)

At [Level 2](../../ROADMAP.md#v04-level-2-your-firmware-as-the-engine-ecu) the NUCLEO-G431KB runs as the engine ECU. A Tang Nano 20K running [open-tick](https://github.com/Open-Heavy-Machinery/open-tick)'s bridge gateware generates the signals the ECU reads (crank, cam, analog sensors) and captures what it commands (injector pulses). This page is how the two boards are wired.

The CAN bus stays wired as in [README.md](README.md#hardware). The pins below do not touch it.

**Status (2026-11-27):**
- The pin map is decided.
- First wiring: crank, cam and the six injectors (column "First wiring").
- The four analog channels come later *(planned)*: their pins and filter are fixed here.
- The reference engine-ECU firmware that uses these pins is *(planned)*.

## Pin map

The Tang pins are those of open-tick's `gateware/tang_nano_20k/tang_nano_20k.cst`. The NUCLEO pins were checked against ST's user manual UM2397 (Rev 2; tables 9, 11 and 12) and the STM32CubeMX pin database for the STM32G431KB.

| Signal | Tang pin | Direction | NUCLEO connector-pin (Arduino) | MCU pin | Peripheral (alternate function) | In series | First wiring |
|---|---|---|---|---|---|---|---|
| Crank, 60−2 wheel | 73 | Tang → NUCLEO | CN3-12 (A0) | PA0 | TIM2_CH1 (AF1), input capture | 220–330 Ω | yes |
| Cam | 74 | Tang → NUCLEO | CN3-11 (A1) | PA1 | TIM2_CH2 (AF1), input capture | 220–330 Ω | yes |
| Injector 1 | 27 | NUCLEO → Tang | CN4-12 (D9) | PA8 | TIM1_CH1 (AF6), one-pulse output compare | 220–330 Ω | yes |
| Injector 2 | 28 | NUCLEO → Tang | CN4-1 (D1) | PA9 | TIM1_CH2 (AF6) | 220–330 Ω | yes |
| Injector 3 | 25 | NUCLEO → Tang | CN4-2 (D0) | PA10 | TIM1_CH3 (AF6) | 220–330 Ω | yes |
| Injector 4 | 26 | NUCLEO → Tang | CN4-15 (D12) | PB4 | TIM3_CH1 (AF2) | 220–330 Ω | yes |
| Injector 5 | 29 | NUCLEO → Tang | CN4-14 (D11) | PB5 | TIM3_CH2 (AF2) | 220–330 Ω | yes |
| Injector 6 | 30 | NUCLEO → Tang | CN4-9 (D6) | PB6 | TIM4_CH1 (AF2) | 220–330 Ω | yes |
| Analog 1 | 76 | Tang → NUCLEO | CN3-9 (A3) | PA4 | ADC2_IN17 | [filter](#analog-filter) | *(planned)* |
| Analog 2 | 86 | Tang → NUCLEO | CN3-6 (A6) | PA7 | ADC2_IN4 | [filter](#analog-filter) | *(planned)* |
| Analog 3 | 72 | Tang → NUCLEO | CN4-6 (D3) | PB0 | ADC1_IN15 | [filter](#analog-filter) | *(planned)* |
| Analog 4 | 71 | Tang → NUCLEO | CN3-8 (A4) | PA5 | ADC2_IN13 | [filter](#analog-filter) | *(planned)*; see [A4 and D4](#a4-and-d4) |
| Ground | GND | | CN4-4 (GND) | | | short, direct wire | yes |
| Filter supply | | | CN3-14 (3V3) | | | 100 nF next to each MCP6022 | *(planned)* |

The Tang's analog channels are `analog[0]` to `analog[3]` in the gateware, and its injector captures `inj[0]` to `inj[5]`.

### Where the Tang pins are on its headers

Seen from above with the USB-C connector at the top (Sipeed's datasheet v1.3, "Pinout"), counting each header from the USB-C end:

| Header | Position | Tang pin | Signal |
|---|---|---|---|
| left | 1 | 73 (IOT40A) | crank |
| left | 2 | 74 (IOT34B) | cam |
| left | 8 | 27 (IOB8A) | injector 1 |
| left | 9 | 28 (IOB8B) | injector 2 |
| left | 10 | 25 (IOB6A) | injector 3 |
| left | 11 | 26 (IOB6B) | injector 4 |
| left | 12 | 29 (IOB14A) | injector 5 |
| left | 13 | 30 (IOB14B) | injector 6 |
| left | 19, 20 | 3V3, GND | ground: position 20; 3V3 is not used |
| right | 1, 2 | 5V, GND | not used (5 V) |
| right | 3 | 76 (IOT30B) | analog 1 |
| right | 13 | 86 (IOT4A) | analog 2 |
| right | 17 | 72 (IOT40B) | analog 3 |
| right | 18 | 71 (IOT44A) | analog 4 |

The first wiring (crank, cam, six injectors, ground) is all on the left header.

**On the NUCLEO side:**
- Crank and cam share TIM2, a 32-bit timer, so the time between teeth never wraps within a revolution at engine speeds. Each input has a pull-down and a small input filter (ICxF).
- The injector outputs are push-pull, at low speed: a slow edge is enough for a capture and rings less on a jumper wire.
- PA9 and PA10 are also USART1. The ECU does not use USART1; its reports go over USART2, the ST-LINK's virtual COM port.

**On the Tang side** (already in the `.cst`):
- Crank, cam and the analog outputs are driven at `DRIVE=8`.
- The injector inputs have `PULL_MODE=DOWN`, so a NUCLEO in reset, with its pins floating, reads as no injection.
- Every header bank is at 3.3 V.

### A4 and D4

On the NUCLEO-G431KB, solder bridge SB2 is closed by default. It joins CN3-8 (A4, PA5) to PB7, which is also CN4-7 (D4). Analog 4 uses A4 with SB2 left as it is:
- PB7 stays in its reset state (analog mode), never an output;
- D4 is not wired.

### Pins not used

| Pins | Why |
|---|---|
| CN4-5 (D2, PA12), CN4-13 (D10, PA11) | the CAN bus (FDCAN1), already wired |
| CN3-5 (A7, PA2), CN3-10 (A2) | the ST-LINK's virtual COM port (USART2). A2 is not even connected to PA3 by default (SB14 open) |
| CN3-7 (A5), CN4-8 (D5) | joined by SB3 (closed by default): PA6 and PA15 |
| CN4-7 (D4, PB7) | A4's partner through SB2 |
| CN4-10, CN4-11 (D7, D8; PF0, PF1) | not connected by default (SB8 and SB11 open); they are the HSE pins |
| CN3-15 (D13, PB3) | SWO through SB4 (closed by default). The user LED is on PB8, not on D13 |

UM2397's table 11 gives D6 as "TIM1_CH1" and D9 as "TIM4_CH1". Its table 12 and the CubeMX database give the opposite, PA8 (D9) TIM1_CH1 and PB6 (D6) TIM4_CH1, which is what this map uses.

## Analog filter

The bridge's analog outputs are first-order sigma-delta bit streams at its 27 MHz clock. Each one becomes a voltage through a unity-gain Sallen-Key low-pass with an MCP6022, a rail-to-rail op-amp powered from the NUCLEO's 3V3:

```
                     C1
              ┌──────||──────┐
              │              │
 in ──R1──────●──R2──●──(+)  │
              A      │    ▷──●──► out
                     C2 ┌(−)  │
                     │  └─────┘
                    GND
```

- **in:** the Tang's analog pin. **out:** the NUCLEO's ADC pin.
- R1 from in to node A; R2 from node A to the op-amp's (+) input.
- C1 from node A to the output (the feedback capacitor); C2 from (+) to ground.
- The output tied to (−): a voltage follower, gain 1.
- R1 = R2 = 10 kΩ and C1 = C2 = 100 nF give Q = 0.5 and a corner frequency of 1/(2π · 10 kΩ · 100 nF) ≈ 159 Hz.
- One MCP6022 (dual) serves two channels, so the four channels need two. Each MCP6022 has its own 100 nF decoupling capacitor between its supply pins.
- Its ground is the NUCLEO's ground, the same wire as the signals'.

## Electrical checks

- **Logic levels.** Both boards are 3.3 V LVCMOS: the Tang's header banks are at 3.3 V, and the STM32G431 runs at VDD = 3.3 V. No level shifter is needed.
- **Ground.** Both boards are powered over USB from the same PC, so they already share a ground through it. The ground wire is still needed: short and direct, so that the signals' return current does not go through the USB cables.
- **One board powered before the other.** An output that drives an unpowered board feeds current into that board through its input protection. The series resistor limits it to about 10 mA at 330 Ω. It limits the current wherever it sits on the line; with short jumper wires, which end does not matter, so all eight can sit on the breadboard where there is room (on the maintainer's bench, next to the NUCLEO).
- **The Tang at power-up.** The bridge keeps crank, cam and the analog outputs low outside a run, and its injector inputs are pulled down. At power-up, the Tang loads the design in its flash, not the one programmed into SRAM. On the maintainer's bench the bridge is in the flash too (`openFPGALoader -b tangnano20k -f bridge.fs`), so the Tang always starts with its outputs low. The measurement scripts still load SRAM before each run.
- **The rule:**
  - power both boards from the same PC;
  - have the bridge loaded before the ECU firmware starts commanding injectors;
  - do not leave one board powered alone with the harness connected.

## Parts for this harness

Beyond the [CAN bench](README.md#hardware):

| Qty | Part | For |
|---|---|---|
| 1 | Tang Nano 20K | the signal bridge (open-tick's gateware) |
| 8 | resistors, 220–330 Ω | in series with crank, cam and each injector |
| 2 | MCP6022 (dual op-amp, DIP-8) | the four analog filters *(planned)* |
| 8 | 10 kΩ resistors | R1 and R2 of the four filters *(planned)* |
| 10 | 100 nF ceramic capacitors | C1 and C2 of the four filters, and one decoupling capacitor per MCP6022 *(planned)* |
| | jumper wires, breadboard | |

## Checking the harness

A continuity check in software is *(planned)*: the NUCLEO counts the crank and cam edges the bridge generates, and sends a different number of pulses on each injector line for the bridge to capture. A crossed or open wire shows as a wrong count on that line.

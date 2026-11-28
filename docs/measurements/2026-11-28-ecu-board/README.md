# The reference engine ECU on the board, at constant speeds (2026-11-28)

open-crank v0.4, Level 2 step 6: `oc-stm32g4-ecu` on the NUCLEO-G431KB against open-tick's Tang Nano 20K bridge, no plant. How the check works and the table of results: [platforms/stm32g4/README.md, "The engine ECU (Level 2)"](../../../platforms/stm32g4/README.md#the-engine-ecu-level-2).

**Run:** `tools/board/ecucheck.py --check build/open-crank-ecu-check --bitstream <open-tick v0.4.0-beta.1>/build-gateware/bridge.fs --out …`, with the defaults (200, 750, 1500 and 2400 rpm, 30 s each after 2 s of warm-up; EEC1 10 s each on `can0`).

- open-crank commit `7dcdb0f205f4fa6649113612b6ac5b19c346e310`; open-tick `v0.4.0-beta.1` (`24fb476`), bitstream md5 `6659c249a3a8c779c0f7bb9cac6e6fa5`, loaded fresh before each speed.
- The Tang on a root USB port (`3-2`, 480 Mbit/s); the harness checked wire by wire before (`wirecheck.py`, 8 of 8 at 1500 rpm).
- The host idle (about 95 % idle before the run, no browser).
- Result: every check ok, no hard run retried.

| File | What |
|---|---|
| `summary.txt` | one line per check, as the script printed it |
| `ecucheck_stdout.txt` | the script's whole output |
| `meta.txt` | commit, bitstream, the Tang's USB path, speeds |
| `check_<rpm>rpm.txt` | `open-crank-ecu-check`'s output with the fixed-fuel build: the hard run, the criteria |
| `captures_<rpm>rpm.csv` | every injection captured: channel, start tick, width, the angle from the bridge's accumulator, the error against TDC − 10°, inside the judged window or not |
| `jitter_<rpm>rpm.csv` | the jitter budget, in the format of `open-crank-l2-run --jitter` |
| `hard_budget_<rpm>rpm.csv` | open-tick's budget of the hard run (`docs/bridge-protocol.md`, "Budget of a hard run") |
| `eec1_<rpm>rpm.candump.log`, `eec1_<rpm>rpm_check.txt` | the reference build's EEC1 frames (`candump -L`, kernel times) and the hard run under them |
| `nucleo.log` | every line from the NUCLEO's USART2, with the host's CLOCK_MONOTONIC time of arrival: both builds' start lines, headers and rows |

`md5sum -c md5.txt` checks the files.

**Earlier the same day,** on commit `460bb93` (the same firmware), a first full run also passed every check, but its 750 rpm hard run ended aborted and was retried, and the retry overwrote the first output; the next commit, `7dcdb0f`, keeps both. One more hard run had ended aborted at about 1.2 s that day, right after the NUCLEO was programmed, out of about a dozen; eight repeats at 750 rpm did not reproduce it. The 87 injections captured before that first abort were all in place; the second abort's output was lost. The cause is not known yet. The hard run ends in open-tick's loop, between the host and the bridge; nothing points to the ECU.

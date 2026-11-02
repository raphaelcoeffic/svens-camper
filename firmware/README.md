# Charger emulator firmware

Emulates the camper's 12V charger on LIN, as described in `../FINDINGS.md`, on the "smart LIN / TTL" adapter board (STM32F103, TJA1021). Whether it unlocks heating is untested.

## What it does

- Answers ID 19 with status `01` from the first poll, then `09` after 7 s. A gap of over 3 s in polling restarts this.
- Reports current from ID 22 at 472 mA per count, or 0 if that's negative, more than 2 s old, or before status `09`.
- Answers 3D with `18 06 F2 34 12 0E 00 01` once, after a 3C request for function `0E`. Stays silent after `0C`.
- Battery protection, reading ID 22 passively: below 11.0 V for 10 s, or with no valid ID 22 for 2 s, it emulates a mains loss as in the traces: `00 01` for 1.7 s, then silence. After a low-voltage cut, it comes back after 60 s above 12.8 V, starting at status `01` with the 7 s delay again. After a lost sensor, it comes back as soon as ID 22 returns. The limits are constants at the top of `charger_emu/main.c`.
- Whether the controller actually stops heating on this sequence is untested.
- To save power, it runs at 2 MHz, sleeps between events and only flashes STA briefly. This halved the draw on the bench supply, from 0.02 A to 0.01 A, as far as its 10 mA resolution shows.

## Board pins

| Pin | Function | Firmware |
|---|---|---|
| PA2, PA3 | TJA1021 TXD, RXD (USART2) | LIN at 19200 8N1 |
| PA11 | TJA1021 SLP_N (likely), held high by the board | Not touched |
| PB9 | 1 kΩ master pull-up, high = on | Low at boot, `pu 1` for bench use only |
| PB8 | Drives the RESET pad, inverted; it isn't the chip's reset | Low |
| PA15 | STA LED | Short flash every 2 s when idle, every 500 ms while answering ID 19 |
| PA9, PA10 | Debug UART (USART1) | 57600 8E1 |

## Bench wiring (RP2040-Zero running MicroPython)

GP12 to board RX, GP13 from board TX, GP11 through 1 kΩ to BOOT0. The board has a 10 kΩ pull-down on BOOT0, so it starts from flash when the RP2040 is absent. Saleae channel 0 on LIN, with the automation server running on port 10430, for the self-test.

## Build, flash, test

```
make -C firmware/charger_emu
uv run --with mpremote python firmware/flash.py
uv run --with logic2-automation --with mpremote python firmware/selftest.py <outdir>
uv run --with mpremote mpremote connect /dev/cu.usbmodem1101 run firmware/consoletest_rp.py
```

`consoletest_rp.py` saves and resets settings on the board, so it ends with defaults.

If the chip doesn't respond to the `rom` request, for example with blank flash, `flash.py` waits for a power cycle.

## Updating with a USB-serial adapter

Connect only RX, TX and GND, with a 3.3V adapter: its TX to board RX, its RX to board TX. BOOT0 can stay low.

```
uv run --with pyserial python firmware/update.py /dev/cu.usbserial-XXXX
```

This sends `rom`, then runs `stm32flash`, which erases, writes, verifies and starts the new firmware. LIN stays recessive and silent during the update, so the controller will see the charger disappear.

If an update is interrupted, the `rom` command may no longer be available. Recovery then needs BOOT0 held high, for example with 3.3V through 1 kΩ, and a power cycle, so keep the BOOT0 wire accessible.

## Console

Open the board's RX/TX at 57600 8N1 with any terminal, for example `screen /dev/cu.usbserial-XXXX 57600`, and type `help`.

| Command | Does |
|---|---|
| `st` | Counters, battery voltage and current |
| `show` | All settings |
| `set <name> <value>` | Change a setting now |
| `save` | Keep settings over a power cycle; this costs a few LIN frames, so don't use it while testing |
| `defaults` | Reset settings, until saved |
| `mon 0\|1` | Print every LIN frame as `id: bytes`, with the checksum last |
| `pu 0\|1` | 1 kΩ master pull-up, bench only, always off at boot |
| `f <id> [hex bytes]` | Send a LIN frame as if master |
| `boot`, `rom`, `hang` | Reset, enter the ROM bootloader, test the watchdog |

| Setting | Default | Meaning |
|---|---|---|
| `mode` | 0 | 0 auto, 1 force mains, 2 force no mains, 3 silent. Never saved, so a power cycle returns to auto. Mode 1 ignores the battery cutoff |
| `cur` | -1 | ID 19 current in counts, -1 = from ID 22 |
| `ma_count` | 472 | mA per ID 19 count |
| `mains_ms` | 7000 | Status `01` time before `09` |
| `cutoff` | 1 | Battery protection on or off |
| `cut_mv`, `cut_s` | 11000, 10 | Cut below this voltage, for this long |
| `rec_mv`, `rec_s` | 12800, 60 | Recover above this voltage, for this long |
| `silent_ms` | 1700 | Status `01` time between a cut and silence |
| `resp_us` | 130 | Reply delay; 130 gives about 370 µs on the bus |
| `stats_s` | 0 | Print the counters this often, 0 off |
| `log` | 1 | Print events such as `status 09` and `cut: low voltage` |
| `sta_on` | 1 | Level that lights the STA LED |

Settings are in the last flash page. `update.py` keeps them, as `stm32flash` only erases the pages it writes; `flash.py` erases the whole chip, which resets them. Adding or removing a setting in the firmware also resets them.

## Limits

- `f` only proves the path through the transceiver's echo, using one UART for both directions.
- `mastertest.py` covers a separate master: a 2N7000 on GP10 (100 Ω to the gate, 10 kΩ gate to GND, drain to LIN, source to GND) replays the controller's schedule, with `pu 1` providing the pull-up. The emulator received frames without errors with the master up to ±4% off in baud rate. That master doesn't read LIN, so how the real controller tolerates the emulator's replies is untested.
- Without a master pull-up, LIN rises too slowly at 19200 for the receive path (32 µs from 1.5 V to 7.2 V, against 3.2 µs with 1 kΩ), so the self-test turns on `pu 1`.
- Replies are typically 370 µs after the header, but printing on the console can delay one to about 1 ms.
- Each reset puts one dominant pulse of about 650 µs on LIN, while PA2 isn't driven yet.
- The clock is the internal oscillator, measured at -0.07% from 19200 baud at room temperature. It hasn't been tested when cold.

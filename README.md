# Sven's camper

Emulates the 12V charger of a Westfalia camper on its LIN bus, so the on-board controller allows electric heating on solar and battery power, without mains. It runs on a reused "smart LIN / TTL" adapter board (STM32F103, TJA1021).

**Status:** tested on the bench against a replay of the controller's schedule, not yet on the camper. Whether it unlocks heating is unknown.

- [`FINDINGS.md`](FINDINGS.md): what the LIN traffic means, from the captures
- [`firmware/`](firmware/README.md): the emulator, with its console, settings, update and test tools
- `decoder/`: the LIN decoder used on the captures
- `decoded/`: the decoded captures, one JSON object per frame
- `Sven-LIN-trace.txt`: serial log of the bus

The original Saleae captures (`.sal`, about 875 MB) aren't in this repository, as they're too large for it.

## License

[MIT](LICENSE)

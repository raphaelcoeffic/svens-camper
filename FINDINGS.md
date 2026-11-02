# Westfalia LIN bus: charger findings

Goal: emulate the 12V charger on LIN so the on-board controller allows electric heating on solar and battery power, without mains.

**Status:** the charger's LIN interface is understood well enough to emulate. It is not yet known whether emulation would unlock heating. The controller currently refuses heating because it sees a charger fault (probably a defective 12V switch), the charger's status byte shows no fault, and no trace contains a heating request.

Confidence labels used below:
- **Measured:** read directly from the traces.
- **Inferred:** a reasonable interpretation that has not been tested.
- **Unknown:** no evidence either way.

## Source traces

| File | Content | Decoded span |
|---|---|---|
| `Sven-LIN-trace.txt` | Serial log, described as taken with the working charger | About 4.4 s + 4.5 s (two runs) |
| `Netzteil-ON.sal` | Half-repaired charger, mains on, no events | 32.2–57.8 s, 1,418 frames |
| `Netzteil-ON-OFF-ON.sal` | Half-repaired charger, mains on, off, on | 0–60.5 s, 3,346 frames |

The `.txt` file holds two separate runs, because its frame numbers overlap and its timestamps jump back at line 951:
- **Lines 1–950:** the charger never answers and ID 18 is not scheduled. This matches the mains-off state, with the battery at 13.70 V and about -180 mA.
- **Lines 951 onward:** the charger is running, at about 18 A and 14.17 V. This is the only real "working charger" reference.

Across both `.sal` captures, all 4,764 frames have valid PIDs. The only bad checksum is the charger's last garbled frame as it loses power (`FF FF` at 14.0 s).

## Physical layer (measured)

- 19200 baud. The Saleae analyzer in both captures is set to 20000.
- LIN 2.x: enhanced checksums, except classic on diagnostic frames 3C/3D.
- With the 3.3V+ digital threshold, dominant pulses look 18–26 µs too short. A bit reads as 34 µs low and 70 µs high, for example. The decoder compensates for this.
- The analog channel appears to clip at about 10.7 V, so it can't show the true bus voltage. Dominant levels are still usable.

## Schedule (measured)

The cycle is 342.5 ms. Offsets are from one cycle of `Netzteil-ON`, with payloads including the checksum byte.

| Offset (ms) | PID | ID | Example payload |
|---|---|---|---|
| 0.0 | 1A | 1A | `CF 00 2A 00 00 00 00 EB` |
| 50.0 | 5B | 1B | `02 00 2F 2F EA 00 00 59` |
| 100.2 | 42 | 02 | no response |
| 110.2 | 20 | 20 | no response |
| 120.3 | E2 | 22 | `52 85 1E 0C 38 62 7A 06` |
| 130.3 | 25 | 25 | `C8 C8 FF B9 FF FF 8F` |
| 140.3 | A6 | 26 | `AC 03 AC 03 2F FE CB` |
| 150.5 | 78 | 38 | no response |
| 160.7 | BA | 3A | no response |
| 201.0 | 4C | 0C | `22 02 00 05 00 00 80 10 F9` |
| 211.6 | 42 | 02 | no response |
| 221.8 | C4 | 04 | no response |
| 231.8 | 85 | 05 | `00 11 00 00 00 00 00 00 69` |
| 241.8 | FB | 3B | `00 00 00 00 00 00 00 00 04` |
| 252.1 | 06 | 06 | no response |
| 262.1 | 97 | 17 | no response |
| 272.3 | EC | 2C | no response |
| 282.5 | D8 | 18 | `50 0C 38 00 92` |
| 292.5 | 99 | 19 | `02 09 5B` |

3C and 3D are not in the regular schedule. They appeared only when the charger came back.

## Nodes (inferred)

Grouped by dominant voltage level and the delay before the response, measured in the ON-OFF-ON capture.

| Node | IDs | Low level (V) | Response delay (µs) |
|---|---|---|---|
| Controller (master) | 1A, 05, 18, 3C | 1.22 | 0 |
| Charger | 19, 3D | 1.05–1.14 | 386–414 |
| Battery sensor and possibly others | 1B, 22, 25, 26 | 1.44–1.47 | 10–21 |
| Unknown slave | 0C | 1.21 | 105–337 |
| Unknown slave | 3B | 1.34 | 126–138 |

- IDs 02, 04, 06, 17, 20, 2C, 38 and 3A got no response in any trace. They might be optional equipment or the heating, but that is a guess.
- All dominant levels drop by about 0.18 V while the charger is off.

## Frame meanings

### ID 19 (PID 99): charger status

Response: `[current, status, checksum]`

- **Current (measured):** about 0.47 A per count, assuming ID 22 current is in mA. The working run gives about 18,000 mA at 38 counts, and a fit over the ON-OFF-ON capture gives 472 mA per count.
- **Status (measured):** `0x09` while the mains-on behaviour is present, `0x01` while the charger is awake without it. The mains link itself is inferred, because the traces have no plug markers.
- **Checksum example:** `0x99 + 0x03 + 0x09 = 0xA5`, inverted gives `0x5A`, so the frame is `03 09 5A`.

### ID 22 (PID E2): battery sensor (inferred)

| Bytes | Meaning | Evidence |
|---|---|---|
| 0–1 | Current in mA, little-endian, offset by 0x8000 | Tracks ID 19 current closely |
| 2 | Unknown | Always `0x1E` (30) |
| 3–4 | Voltage in mV, little-endian | 13.70–14.40 V range |
| 5 | Probably state of charge | 98–100 |
| 6 | Unknown | `0x7A` or `0x7C`, and `0x7C` tends to appear at higher current |

While the charger is off, this current still reads about +220 mA. The cause is unknown, and solar is one possibility.

### ID 18 (PID D8): voltage from the controller (inferred)

`50 <voltage low> <voltage high> 00 <checksum>` repeats the ID 22 voltage. The controller sends it only while the charger is alive, so it is probably addressed to the charger.

### 3C/3D: charger identity (measured)

Request: `18 06 B2 00 34 12 <function> 00`. This is a LIN "read by identifier" to NAD 0x18, with supplier 0x1234.

Response: `18 06 F2 34 12 0E 00 01`, meaning function 0x000E, variant 01.

### Other frames

The meanings of IDs 1A, 1B, 25, 26, 0C, 05 and 3B are unknown.

## Mains off and on sequence (measured)

From `Netzteil-ON-OFF-ON`. The plug and unplug moments are inferred from the bus.

| Time (s) | Event |
|---|---|
| 12.287 | ID 19 becomes `00 01 65` (mains off) |
| 13.999 | Garbled `FF FF`, the last sign of the charger |
| 14.342 | First ID 19 header with no response |
| 17.072 | Last ID 18 header before the gap |
| 26.731 | Charger answers again: `01 01 64` |
| 26.992 | 3C with function `0C`; the following 3D gets no response |
| 27.344 | 3C with function `0E` |
| 27.354 | 3D answers `18 06 F2 34 12 0E 00 01` |
| 27.435 | ID 18 resumes |
| 33.611 | ID 19 becomes `00 09 5D` (status `09`) |
| 34.0 | Current jumps to 25–26 counts, then tapers to 2–5 counts by about 40 s |

The charger's LIN node runs only with mains connected, and is silent otherwise.

## Working charger vs half-repaired

Compares the charging run in the `.txt` file with `Netzteil-ON`.

| Frame | Working | Half-repaired | Comment |
|---|---|---|---|
| ID 19 status | `09` | `09` | **No visible fault flag** |
| ID 1A byte 2 | `00` | `2A` | **Top suspect.** Sent by the controller; unchanged through mains off and on |
| ID 0C byte 1 | `03` | `02` | **Second suspect** |
| ID 19 current | 37–39 | 1–5 | May only reflect a fuller battery, since it starts at 25–26 after replugging |
| ID 1B | `02 00 31 31 C0 00 00` | `02 00 2F 2F EA 00 00` | Unknown |
| ID 25 byte 0 | `C6` | `C8` | Unknown |
| ID 26 bytes 2–3 | `A4 03` | `AC 03` | Unknown |
| Battery | 14.17 V, about +18 A | 14.35 V, +1.1 to +3.0 A | Different battery state |

The half-repaired charger does deliver current, and the battery sensor confirms it.

The cause of the fault is unknown:
- `1A = 2A` could be a fault code or simply a setting.
- If the fault comes from a hard-wired signal or a stored fault, a perfect LIN emulation will not clear it.

## Charger disconnected from LIN (measured)

From `NoCharger-mains-on.sal`, 30 s with mains plugged, the charger's LIN wire disconnected, and no button presses. All 1,626 frames decoded with valid PIDs and checksums.

- **ID 19:** the header is still sent, but nothing answers.
- **ID 18:** never sent, as in the mains-off state.
- **3C/3D:** never sent, so the controller doesn't probe for an absent charger during these 30 s.
- **ID 1A byte 2:** stays `2A`.
- **ID 0C byte 1:** stays `02`.
- **Other responding frames:** all unchanged, apart from the live readings in ID 22.
- **Battery:** about 14.35 V and +0.5 to +1.1 A at the first and last readings. This suggests the charger keeps charging without LIN, though solar can't be ruled out.

So `2A` and `02` don't clear when the charger drops off LIN. The controller was not restarted for this capture, so they could still be stored values.

### Heating button without the charger

From `NoCharger-heating.sal`, 60 s in the same state, with the heating button pressed once. The press time was not recorded. All 3,253 frames decoded with valid PIDs and checksums.

- **Panel message:** "No external power plugged", even though mains was plugged in.
- **Bus reaction (measured):** no response payload changed, and no new IDs appeared. The only anomaly is ID 0C going unanswered twice, at 58.96 s and 59.96 s. It also missed one response in `Netzteil-ON-OFF-ON`, so this isn't specific to the button.
- **Battery current (measured):** steady at about 1.1–1.5 A at the 5 s sample points, so no visible load change.

Inferences:
- The controller seems to learn about mains from the charger over LIN, because real mains alone was not recognised. ID 19 status `0x09` is the likely signal. A separate mains input could still exist as an additional condition.
- The request is refused without any visible LIN traffic, so this capture doesn't show what a real heating request looks like.

### Two different refusals

| Charger state | Panel message | Likely meaning |
|---|---|---|
| LIN disconnected, mains plugged | "No external power plugged" | Controller sees no charger, so no mains |
| Half-repaired charger connected | Roughly "to protect the electric circuit, heating is disabled" (wording from memory, no capture) | Controller sees the charger but blocks heating |

So there seem to be two conditions: mains must be reported, and some protection check must pass.

- **Not explained by status:** ID 19 status is `09` in both the working and half-repaired traces.
- **ID 1A `2A` alone can't select the message:** it is present in both states. It could still be the protection condition if the missing-mains message takes priority.
- **Remaining candidates:** ID 19 current (37–39 working, 1–5 half-repaired), ID 1A byte 2, ID 0C byte 1, or a hard-wired signal that LIN can't show.
- **Safety:** this check may be a real protection, so find out what it guards before bypassing it.

Restarting the controller is not an option, because it would mean disconnecting the solar panels. Whether `2A` is stored remains unknown.

## Minimal emulator (untested)

1. Disconnect the real charger from LIN, because two nodes can't both answer ID 19.
2. Answer ID 19 with `[current, 0x01, checksum]` for about 7 s, then `[current, 0x09, checksum]`. Use the enhanced checksum with PID 99.
3. Answer 3D with `18 06 F2 34 12 0E 00 01` (classic checksum `99`) only after a 3C request with function `0E`. Stay silent after function `0C`.
4. Report a current consistent with ID 22, in case the controller cross-checks the two.

Electrical cautions:
- The heater runs from the 12V batteries, even with the charger connected (stated by the owner). Its current draw is not known yet.
- With the emulator, nothing replaces the charger's input, so the load comes from the batteries and solar alone. Whether the controller then protects the batteries is unknown. The emulator reports a mains loss below 11.8 V, but whether that stops the heater is untested.
- With a load on, battery current no longer equals charger current. The emulator derives ID 19 current from ID 22, so it would report 0 while the heater draws more than the solar supplies. How the controller reacts is unknown.

An implementation for a reused adapter board is in `firmware/`. It has passed a bench self-test, not yet with an external master or on the camper.

## Traces still needed

Capture the digital channel and note the time of each action.

1. **Heating request:** press the heating button with the faulty charger, and with a working one if possible. This is the most useful capture, as it shows which frames carry the request.
2. **Charger unplugged from LIN:** with mains on and off, to see whether ID 1A and ID 0C change.
3. **Known-good reference:** another Westfalia, if available, to separate faults from settings.

A capture of the charger's LIN wire being reconnected is not practical, because it would mean resoldering to a different tap. `Netzteil-ON-OFF-ON.sal` already covers most of it: the charger drops off and returns, the controller probes it with 3C, and ID 1A and ID 0C don't change. The gap is a button press with the charger connected.

## Files and decoding

- `decoder/lin.py`: decoder that works from exported digital edges.
- `decoded/Netzteil-ON.json`, `decoded/Netzteil-ON-OFF-ON.json`: one object per frame.

Export the edges with the Logic2 automation API, using `export_raw_data_csv(directory=..., digital_channels=[4])`. Wait until the capture has finished loading first: an export of `Netzteil-ON-OFF-ON.sal` made right after opening it stopped at 2.4 s, and the complete one reaches 60.5 s. Then decode:

```python
import lin
t, v = lin.load("digital.csv")   # columns: time, then channel state
frames = lin.decode(t, v)        # each frame: t, brk, bytes, errs, times, skews
```

`lin.decode` returns raw frames including the sync byte. The JSON files were built from those by a separate analysis script that is not in this project.

JSON fields:

| Field | Meaning |
|---|---|
| `t` | Capture time of the break (s) |
| `pid`, `id` | Protected ID and frame ID |
| `pidok` | PID parity is valid |
| `d` | Response bytes including the checksum; empty if no response |
| `errs` | Framing errors in the frame |
| `cs` | Last response byte (the checksum) |
| `classic`, `enh` | Checksum matches the classic or enhanced calculation |
| `sk`, `hsk` | Measured pulse skew of the response and header (µs) |
| `gap` | Delay from the end of the PID byte to the first response byte (µs) |

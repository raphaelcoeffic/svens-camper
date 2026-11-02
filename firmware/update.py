#!/usr/bin/env python3
"""Updates the firmware over a 3.3V USB-serial adapter, with only RX, TX and GND connected.

Asks the running firmware to enter the ROM bootloader, then runs stm32flash, which erases, writes,
verifies and starts the new firmware.

Usage: uv run --with pyserial python firmware/update.py /dev/cu.usbserial-XXXX [firmware.bin]
"""
import pathlib
import subprocess
import sys
import time
import serial

port = sys.argv[1]
image = sys.argv[2] if len(sys.argv) > 2 else pathlib.Path(__file__).parent / "charger_emu/charger_emu.bin"
with serial.Serial(port, 57600, timeout=0.5) as s:
    s.write(b"\nrom\n")
    s.flush()
    print("firmware said:", s.read(32))
time.sleep(0.2)
sys.exit(subprocess.run(["stm32flash", "-b", "57600", "-w", str(image), "-v", "-g", "0x08000000", port]).returncode)

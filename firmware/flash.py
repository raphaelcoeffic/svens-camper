#!/usr/bin/env python3
"""Flashes a binary through the STM32 ROM bootloader, with the RP2040 (MicroPython) as the bridge.

GP12 to board RX, GP13 from board TX, GP11 through 1k to BOOT0. Sets BOOT0 high and asks running
firmware to enter the bootloader ("rom" on the debug UART); a chip that doesn't answer needs a power
cycle, which this waits for. Erases, writes, verifies, sets BOOT0 low and starts the firmware.

Usage: uv run --with mpremote python firmware/flash.py [--boot0-low] [firmware.bin]
--boot0-low keeps BOOT0 low throughout, relying on the "rom" command alone.
"""
import glob
import pathlib
import subprocess
import sys
import tempfile

BRIDGE = r'''
import time
from machine import UART, Pin
from binascii import unhexlify
BIN=unhexlify("@@HEX@@")
BASE=0x08000000
u=UART(0,57600,bits=8,parity=0,stop=1,tx=Pin(12),rx=Pin(13),rxbuf=2048,timeout=0)
ACK=b"\x79"
def rd(n,ms=500):
    b=bytearray(); t=time.ticks_ms()
    while len(b)<n and time.ticks_diff(time.ticks_ms(),t)<ms:
        c=u.read(n-len(b))
        if c: b+=c
    return bytes(b)
def flush():
    time.sleep_ms(20)
    while u.any(): u.read()
def cmd(c):
    flush(); u.write(bytes([c,c^0xFF])); return rd(1)
def addr(a):
    ab=a.to_bytes(4,"big"); x=0
    for v in ab: x^=v
    u.write(ab+bytes([x])); return rd(1)
def fail(msg):
    print("FAILED:",msg); raise SystemExit
Pin(11,Pin.OUT,value=@@BOOT0@@)
time.sleep_ms(5)
# The console is 8N1; firmware before 1.1 used 8E1, as the bootloader does
u.init(57600,bits=8,parity=None,stop=1,tx=Pin(12),rx=Pin(13))
u.write(b"\nrom\n"); time.sleep_ms(50)
u.init(57600,bits=8,parity=0,stop=1,tx=Pin(12),rx=Pin(13))
u.write(b"\nrom\n"); time.sleep_ms(200)
print("waiting for the bootloader (power-cycle the board if this doesn't finish)")
t0=time.ticks_ms(); up=False
while time.ticks_diff(time.ticks_ms(),t0)<170000:
    flush(); u.write(b"\x7f"); s=rd(1,300)
    if s in (ACK,b"\x1f") and cmd(0x02)==ACK:
        print("bootloader ID",rd(4)); up=True; break
    time.sleep_ms(300)
if not up: fail("no bootloader")
if cmd(0x43)!=ACK: fail("erase refused")
u.write(b"\xff\x00")
if rd(1,20000)!=ACK: fail("erase")
print("erased")
for o in range(0,len(BIN),256):
    c=BIN[o:o+256]
    if cmd(0x31)!=ACK or addr(BASE+o)!=ACK: fail("write refused at %d"%o)
    x=len(c)-1
    for v in c: x^=v
    u.write(bytes([len(c)-1])+c+bytes([x]))
    if rd(1,2000)!=ACK: fail("write at %d"%o)
for o in range(0,len(BIN),256):
    n=min(256,len(BIN)-o)
    if cmd(0x11)!=ACK or addr(BASE+o)!=ACK: fail("read refused at %d"%o)
    u.write(bytes([n-1,(n-1)^0xFF]))
    if rd(1)!=ACK or rd(n,1500)!=BIN[o:o+n]: fail("verify at %d"%o)
print("written and verified",len(BIN),"bytes")
# A "rom" request that met BOOT0 high went straight to ROM, leaving the firmware's request flag set
if cmd(0x31)!=ACK or addr(0x20004FFC)!=ACK: fail("flag clear refused")
u.write(bytes([3,0,0,0,0,3]))
if rd(1,1000)!=ACK: fail("flag clear")
Pin(11,Pin.OUT,value=0)
if cmd(0x21)!=ACK or addr(BASE)!=ACK: fail("Go refused")
print("started")
u.init(57600,bits=8,parity=None,stop=1,tx=Pin(12),rx=Pin(13))
t=time.ticks_ms(); b=bytearray()
while time.ticks_diff(time.ticks_ms(),t)<800:
    c=u.read(64)
    if c: b+=c
print(bytes(b))
'''


def main():
    args = sys.argv[1:]
    boot0 = "0" if "--boot0-low" in args else "1"
    args = [a for a in args if a != "--boot0-low"]
    path = pathlib.Path(args[0] if args else pathlib.Path(__file__).parent / "charger_emu/charger_emu.bin")
    data = path.read_bytes()
    data += b"\xff" * (-len(data) % 4)
    port = glob.glob("/dev/cu.usbmodem*")[0]
    with tempfile.NamedTemporaryFile("w", suffix=".py") as f:
        f.write(BRIDGE.replace("@@HEX@@", data.hex()).replace("@@BOOT0@@", boot0))
        f.flush()
        sys.exit(subprocess.run(["mpremote", "connect", port, "run", f.name]).returncode)


if __name__ == "__main__":
    main()

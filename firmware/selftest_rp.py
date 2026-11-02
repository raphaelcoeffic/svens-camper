# Runs on the RP2040: drives the emulator's "f" self-test command and prints its debug output.
import time
from machine import UART, Pin
u=UART(0,57600,bits=8,parity=None,stop=1,tx=Pin(12),rx=Pin(13),rxbuf=2048,timeout=0)
Pin(11,Pin.OUT,value=0)
t0=time.ticks_ms()
buf=bytearray()
def pump(ms):
    global buf
    t=time.ticks_ms()
    while time.ticks_diff(time.ticks_ms(),t)<ms:
        c=u.read(64)
        if c: buf+=c
        while b"\n" in buf:
            l,_,r=bytes(buf).partition(b"\n"); buf=bytearray(r)
            print("%7.3f"%(time.ticks_diff(time.ticks_ms(),t0)/1000),"".join(chr(v) if 32<=v<127 else "?" for v in l))
def send(s,ms=60):
    print("%7.3f"%(time.ticks_diff(time.ticks_ms(),t0)/1000),">",s)
    u.write(s+"\n"); pump(ms)
DIAG="f 3C 18 06 B2 00 34 12 %s 00"
BATT="f 22 %02X %02X 1E %02X %02X 62 7A"
def batt(lo,hi,mv=14348): return BATT%(lo,hi,mv&255,mv>>8)
pump(100)
send("boot",800)                    # reset with BOOT0 low: restarts from flash, fresh state
send("pu 1",100)                    # the bench bus has no master: with 30k alone, edges are too slow
send(DIAG%"0C"); send("f 3D")       # no answer expected
send(DIAG%"0E"); send("f 3D")       # answer expected
send("f 3D")                        # one-shot: no answer expected
for i in range(30):                 # about 10 s of polling: status 01, then 09
    send(batt(0x52,0x85),170); send("f 19",170)      # +1362 mA
for lo,hi,name in ((0x00,0xC0,"+16384 mA"),(0x00,0x70,"-4096 mA")):
    print("battery",name)
    for i in range(3):
        send(batt(lo,hi),170); send("f 19",170)
def phase(name,mv,seconds):
    print(name)
    for i in range(int(seconds/0.34)):
        send(batt(0x52,0x85,mv),170); send("f 19",170)
print("silence 3.5 s"); pump(3500)  # ID 22 lost too: cuts, then restarts once it's back
for i in range(8): send("f 19",340)
phase("back, 14.3 V",14348,9)
phase("dip 10.5 V for 5 s: no cut",10500,5)
phase("14.3 V",14348,2)
phase("10.5 V for 14 s: cut after 10 s, silent 1.7 s later",10500,14)
phase("12.5 V for 5 s: stays cut",12500,5)
phase("13 V for 62 s: recovers after 60 s",13000,62)
phase("13 V: back to 09",13000,9)
send("st",200)
send("hang",600)                    # the watchdog should restart it
send("pu 1",100)
for i in range(3): send("f 19",340)
send("pu 0",100)
print("END")

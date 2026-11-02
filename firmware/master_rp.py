# Runs on the RP2040: a bench LIN master through a 2N7000 on GP10 (GP10 high = dominant), replaying
# the controller's schedule from FINDINGS.md. Debug UART to the emulator on GP12/GP13.
import rp2, time
from array import array
from machine import UART, Pin, mem32
from rp2 import asm_pio, PIO
u=UART(0,57600,bits=8,parity=None,stop=1,tx=Pin(12),rx=Pin(13),rxbuf=2048,timeout=0)
Pin(11,Pin.OUT,value=0)
def pump(ms):
    t=time.ticks_ms(); b=bytearray()
    while time.ticks_diff(time.ticks_ms(),t)<ms:
        c=u.read(64)
        if c: b+=c
    return bytes(b)
def show(b):
    for l in b.split(b"\n"):
        if l: print(" ","".join(chr(v) if 32<=v<127 else "?" for v in l))
def pid_of(i):
    b=[(i>>k)&1 for k in range(6)]
    return i|((b[0]^b[1]^b[2]^b[4])<<6)|((1^b[1]^b[3]^b[4]^b[5])<<7)
def ck(data,seed):
    s=seed
    for d in data:
        s+=d
        if s>255: s-=255
    return (~s)&255
@asm_pio(out_init=PIO.OUT_HIGH,out_shiftdir=PIO.SHIFT_RIGHT,autopull=True,pull_thresh=32,fifo_join=PIO.JOIN_TX)
def bits():
    out(pins,1)[7]
def stream(i,data=None):
    """Bit stream, least significant first: break, delimiter, sync, PID, data, checksum, then idle."""
    v=0; n=13                    # 13 dominant
    v|=3<<n; n+=2                # delimiter
    def byte(x):
        nonlocal v,n
        v|=((x<<1)|(1<<9))<<n; n+=10
    byte(0x55); byte(pid_of(i))
    if data:
        for d in data: byte(d)
        byte(ck(data,0 if i in (0x3C,0x3D) else pid_of(i)))
    words=(n+31)//32
    v|=((1<<(words*32-n))-1)<<n  # idle (recessive) to the end of the last word
    return array("I",[(v>>(32*k))&0xFFFFFFFF for k in range(words)])
# ID, offset ms, payload from another node or the controller (None: header only)
BATT=[0x52,0x85,0x1E,0x0C,0x38,0x62,0x7A]
SCHED=[(0x1A,0.0,[0xCF,0,0x2A,0,0,0,0]),(0x1B,50.0,[2,0,0x2F,0x2F,0xEA,0,0]),(0x02,100.2,None),(0x20,110.2,None),
 (0x22,120.3,BATT),(0x25,130.3,[0xC8,0xC8,0xFF,0xB9,0xFF,0xFF]),(0x26,140.3,[0xAC,3,0xAC,3,0x2F,0xFE]),(0x38,150.5,None),
 (0x3A,160.7,None),(0x0C,201.0,[0x22,2,0,5,0,0,0x80,0x10]),(0x02,211.6,None),(0x04,221.8,None),(0x05,231.8,[0,0x11,0,0,0,0,0,0]),
 (0x3B,241.8,[0]*8),(0x06,252.1,None),(0x17,262.1,None),(0x2C,272.3,None),(0x18,282.5,[0x50,0x0C,0x38,0]),(0x19,292.5,None)]
def run(baud,cycles):
    sm=rp2.StateMachine(0,bits,freq=8*baud,out_base=Pin(10))
    a=0x40014000+8*10+4
    mem32[a]=(mem32[a]&~(3<<8))|(1<<8)   # output inverted: 2N7000 gate
    sm.active(1)
    frames=[(t,stream(i,d)) for i,t,d in SCHED]
    def diag(fn): return stream(0x3C,[0x18,6,0xB2,0,0x34,0x12,fn,0])
    def wait(ms):
        while time.ticks_diff(time.ticks_ms(),t0)<ms: pass
    show(pump(0))
    for c in range(cycles):
        t0=time.ticks_ms()
        for t,s in frames:
            wait(t); sm.put(s)
        wait(310)
        if c==1: sm.put(diag(0x0C)); wait(320); sm.put(stream(0x3D))
        if c==2: sm.put(diag(0x0E)); wait(320); sm.put(stream(0x3D))
        wait(342.5)
        show(pump(0))
    time.sleep_ms(20)
    sm.active(0); rp2.PIO(0).remove_program(); Pin(10,Pin.IN,None)
import sys
for baud,cycles in @@PLAN@@:
    print("=== master",baud,"baud (%+.1f%%)"%((baud/19200-1)*100))
    u.write("boot\n"); show(pump(500))
    u.write("pu 1\n"); show(pump(100))
    run(baud,cycles)
    u.write("st\n"); show(pump(100))
u.write("pu 0\n"); show(pump(100))
print("END")

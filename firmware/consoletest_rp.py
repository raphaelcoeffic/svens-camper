# Runs on the RP2040: exercises the console, settings and persistence. Debug UART on GP12/GP13.
import time
from machine import UART, Pin
u=UART(0,57600,bits=8,parity=None,stop=1,tx=Pin(12),rx=Pin(13),rxbuf=2048,timeout=0)
Pin(11,Pin.OUT,value=0)
def pump(ms):
    t=time.ticks_ms(); b=bytearray()
    while time.ticks_diff(time.ticks_ms(),t)<ms:
        c=u.read(64)
        if c: b+=c
    return "".join(chr(v) if v<127 else "?" for v in b)
fails=[]
def send(s,ms=250):
    u.write(s+"\r"); return pump(ms)
def check(name,cond,got=""):
    print(("ok   " if cond else "FAIL ")+name, "" if cond else repr(got))
    if not cond: fails.append(name)
def setting(name):
    r=send("show",500)
    for l in r.split("\n"):
        if l.startswith(name+" = "): return l.split(" = ")[1].split(" ")[0]
pump(50)
r=send("boot",700); check("boot banner","charger_emu 1.1" in r,r)
send("defaults"); send("save",400)
r=send("boot",700); check("settings saved banner","settings: saved" in r,r)
r=send("help",600); check("help lists commands","hang" in r and "mon 0|1" in r,r)
r=send("show",600); check("show lists all settings","sta_on" in r and "mode = 0" in r,r)
r=send("set nope 1"); check("unknown setting","no such setting" in r,r)
r=send("set cut_mv 1"); check("range check","range 6000 to 20000" in r,r)
r=send("set mode 7"); check("range check on mode","range 0 to 3" in r,r)
r=send("bogus"); check("unknown command","unknown" in r,r)
check("rejected values unchanged",setting("cut_mv")=="11800",setting("cut_mv"))
r=send("set cut_mv 11500"); check("set",setting("cut_mv")=="11500",r)
send("set mode 2"); send("set cur 5")
r=send("save",400); check("save","saved" in r,r)
send("boot",700)
check("saved value survives a reset",setting("cut_mv")=="11500")
check("cur survives too",setting("cur")=="5")
check("mode isn't kept over a reset",setting("mode")=="0")
send("defaults"); check("defaults",setting("cut_mv")=="11800")
send("boot",700)
check("defaults are not saved by themselves",setting("cut_mv")=="11500")
send("defaults"); r=send("save",400); send("boot",700)
check("back to defaults",setting("cut_mv")=="11800" and setting("cur")=="-1")
# Forced modes and the monitor, on the transceiver's echo
send("pu 1",100)
r=send("mon 1",100)
send("f 22 52 85 1E 0C 38 62 7A",60)
r=send("f 19",150); check("mon prints the battery frame","22: 52 85 1E 0C 38 62 7A" in r,r)
r=send("f 1A",150); check("mon shows ID 19 reply","19: 00 01 65" in r,r)
send("set mode 1",100); send("f 19",100); r=send("f 1A",150); check("mode 1 forces 09","19: 00 09 5D" in r or "19: 02 09 5B" in r,r)
send("set cur 7",100); send("f 19",100); r=send("f 1A",150); check("cur fixes the current","19: 07 09" in r,r)
send("set cur -1",100)
send("set mode 2",100); send("f 19",100); r=send("f 1A",150); check("mode 2 forces 01","19: 00 01 65" in r,r)
send("set mode 3",100); send("f 19",100); r=send("f 1A",150); check("mode 3 is silent","19:\r" in r,r)
send("set mode 0",100)
send("mon 0",100); send("pu 0",100)
# A long print must not lose LIN frames
r=send("st",200); check("st",  "hdr=" in r,r)
print("RESULT", "all ok" if not fails else fails)

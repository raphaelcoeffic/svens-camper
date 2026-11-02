"""LIN decoder working from an edge list. Rising edges cross the logic threshold early, so
dominant pulses look too short; rising edges are moved later by a fixed skew before sampling."""
import csv,bisect,statistics
BIT=1/19200
def load(path):
    t=[];v=[]
    rows=csv.reader(open(path)); next(rows)
    for r in rows:
        t.append(float(r[0])); v.append(int(r[1]))
    out_t=[t[0]];out_v=[v[0]]
    for a,b in zip(t[1:],v[1:]):
        if b!=out_v[-1]: out_t.append(a); out_v.append(b)
    return out_t,out_v
def pid_ok(p):
    i=p&0x3f; b=[(i>>k)&1 for k in range(6)]
    p0=b[0]^b[1]^b[2]^b[4]; p1=1-(b[1]^b[3]^b[4]^b[5])
    return p==(i|p0<<6|p1<<7)
def ck(data,seed=0):
    s=seed
    for d in data:
        s+=d
        if s>255:s-=255
    return (~s)&255
SKEW=18e-6
def decode(t,v):
    """Return frames: dict(t,brk,bytes,errs,times,txskew) where txskew[i] is the rising-edge skew seen in byte i."""
    ct=[a+SKEW if s==1 else a for a,s in zip(t,v)]
    def level(x):
        k=bisect.bisect_right(ct,x)-1
        return v[k] if k>=0 else 1
    n=len(t)
    breaks=[k for k in range(n-1) if v[k]==0 and t[k+1]-t[k]>=500e-6]
    frames=[]
    for bi,k in enumerate(breaks):
        tlim=t[breaks[bi+1]] if bi+1<len(breaks) else t[-1]+1
        b=[];errs=0;times=[];skews=[]
        pos=ct[k+1]
        while True:
            m=bisect.bisect_right(ct,pos)
            while m<n and v[m]!=0: m+=1
            if m>=n or t[m]>=tlim-1e-6: break
            st=ct[m]; byte=0
            for q in range(8):
                byte|=level(st+(1.5+q)*BIT)<<q
            if level(st+9.5*BIT)==0: errs+=1
            # skew: how much low pulses inside this byte are shorter than a whole number of bits
            sk=[];q=m
            while q+1<n and t[q]<st+10*BIT:
                if v[q]==0:
                    w=t[q+1]-t[q]; sk.append(round((w+SKEW)/BIT)*BIT-w)
                q+=1
            b.append(byte);times.append(st);skews.append(statistics.median(sk) if sk else None)
            pos=st+9.5*BIT
        frames.append(dict(t=t[k],brk=t[k+1]-t[k],bytes=b,errs=errs,times=times,skews=skews))
    return frames

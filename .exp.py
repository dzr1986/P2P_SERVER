def rotl8(x,n): return ((x<<n)|(x>>(8-n)))&0xff
def gmul(a,b):
    r=0
    for _ in range(8):
        if b&1: r^=a
        hi=a&0x80
        a=(a<<1)&0xff
        if hi: a^=0x1b
        b>>=1
    return r
def ginv(a):
    res=1; base=a; e=254
    while e:
        if e&1: res=gmul(res,base)
        base=gmul(base,base)
        e>>=1
    return res
sbox=[0]*256
for i in range(256):
    x=0 if i==0 else ginv(i)
    s=x^rotl8(x,1)^rotl8(x,2)^rotl8(x,3)^rotl8(x,4)^0x63
    sbox[i]=s&0xff
RCON=[0,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36]
key=bytes.fromhex("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")
rk=[int.from_bytes(key[i*4:i*4+4],'big') for i in range(8)]
for i in range(8,60):
    t=rk[i-1]
    if i%8==0:
        rot=((t<<8)|(t>>24))&0xffffffff
        b0=sbox[(rot>>24)&0xff]; b1=sbox[(rot>>16)&0xff]; b2=sbox[(rot>>8)&0xff]; b3=sbox[rot&0xff]
        t=((b0<<24)|(b1<<16)|(b2<<8)|b3)
        t ^= RCON[i//8]<<24
    elif i%8==4:
        b0=sbox[(t>>24)&0xff]; b1=sbox[(t>>16)&0xff]; b2=sbox[(t>>8)&0xff]; b3=sbox[t&0xff]
        t=((b0<<24)|(b1<<16)|(b2<<8)|b3)
    rk.append(rk[i-8]^t)
# print round keys 0..4 (each 16 bytes = 4 words)
for r in range(5):
    w = rk[r*4:r*4+4]
    h = "".join("%08x"%x for x in w)
    print("round key %d: %s" % (r, h))

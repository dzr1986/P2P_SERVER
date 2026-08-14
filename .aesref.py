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

def rotl8(x,n): return ((x<<n)|(x>>(8-n)))&0xff

sbox=[0]*256; inv=[0]*256
for i in range(256):
    x=0 if i==0 else ginv(i)
    s=x^rotl8(x,1)^rotl8(x,2)^rotl8(x,3)^rotl8(x,4)^0x63
    sbox[i]=s&0xff
    inv[s]=i

RCON=[0,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36]

key=bytes.fromhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4")
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

def rk_byte(i): return (rk[i>>2]>>(24-8*(i&3)))&0xff

pt=bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")
st=[pt[i]^rk_byte(i) for i in range(16)]
print("state after addkey0:", bytes(st).hex())
for round in range(1,14):
    st=[sbox[x] for x in st]
    t=[0]*16
    t[0]=st[0];  t[1]=st[5];  t[2]=st[10];  t[3]=st[15]
    t[4]=st[4];  t[5]=st[9];  t[6]=st[14];  t[7]=st[3]
    t[8]=st[8];  t[9]=st[13]; t[10]=st[2];  t[11]=st[7]
    t[12]=st[12]; t[13]=st[1]; t[14]=st[6]; t[15]=st[11]
    for c in range(4):
        a0=t[c]; a1=t[4+c]; a2=t[8+c]; a3=t[12+c]
        st[c]=gmul(a0,2)^gmul(a1,3)^a2^a3
        st[4+c]=a0^gmul(a1,2)^gmul(a2,3)^a3
        st[8+c]=a0^a1^gmul(a2,2)^gmul(a3,3)
        st[12+c]=gmul(a0,3)^a1^a2^gmul(a3,2)
    st=[x^rk_byte(16*round+i) for i,x in enumerate(st)]
st=[sbox[x] for x in st]
t=[0]*16
t[0]=st[0];  t[1]=st[5];  t[2]=st[10];  t[3]=st[15]
t[4]=st[4];  t[5]=st[9];  t[6]=st[14];  t[7]=st[3]
t[8]=st[8];  t[9]=st[13]; t[10]=st[2];  t[11]=st[7]
t[12]=st[12]; t[13]=st[1]; t[14]=st[6]; t[15]=st[11]
ct=[t[i]^rk_byte(16*14+i) for i in range(16)]
print("ciphertext:", bytes(ct).hex())
print("expected:  f58c4c04d6e5f1ba779eabfb5f7bfbd6")

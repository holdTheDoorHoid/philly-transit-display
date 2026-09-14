# Minimal protobuf wire decoder (no deps) - mirrors what the device will do.
def varint(b,i):
    r=0;s=0
    while True:
        c=b[i];i+=1;r|=(c&0x7f)<<s;s+=7
        if not c&0x80: return r,i
def fields(b):
    i=0;out=[]
    while i<len(b):
        k,i=varint(b,i);f=k>>3;wt=k&7
        if wt==0: v,i=varint(b,i)
        elif wt==1: v=b[i:i+8];i+=8
        elif wt==2: l,i=varint(b,i);v=b[i:i+l];i+=l
        elif wt==5: v=b[i:i+4];i+=4
        else: raise ValueError(wt)
        out.append((f,wt,v))
    return out
def msg(b): 
    d={}
    for f,wt,v in fields(b): d.setdefault(f,[]).append(v)
    return d
data=open('tu.pb','rb').read()
top=msg(data)
hdr=msg(top[1][0]); print('header version',hdr.get(1,[b''])[0],'ts',hdr.get(3))
ents=top[2]; print('entities',len(ents),'max bytes',max(map(len,ents)),'avg',sum(map(len,ents))//len(ents))
n17=0
for e in ents:
    em=msg(e); eid=em[1][0].decode()
    if 3 not in em: continue
    tu=msg(em[3][0]); trip=msg(tu[1][0])
    route=trip.get(5,[b''])[0].decode(); tid=trip.get(1,[b''])[0].decode()
    if route!='17': continue
    n17+=1
    veh=msg(tu[3][0]) if 3 in tu else {}
    stus=tu.get(2,[])
    print(f'entity {eid} trip_id={tid} dir={trip.get(6)} rel={trip.get(4)} veh={veh.get(1,[b""])[0].decode()} label={veh.get(2,[b""])[0].decode()} ts={tu.get(4)} delay={tu.get(5)} n_stu={len(stus)}')
    for s in stus[:2]+[x for x in stus if msg(x).get(4,[b''])[0] in (b'21332',b'21297')]:
        sm=msg(s); arr=msg(sm[2][0]) if 2 in sm else {}; dep=msg(sm[3][0]) if 3 in sm else {}
        print(f'   stu seq={sm.get(1)} stop={sm.get(4,[b""])[0].decode()} arr(delay={arr.get(1)},time={arr.get(2)}) dep(delay={dep.get(1)},time={dep.get(2)}) rel={sm.get(5)}')
print('route17 entities',n17)

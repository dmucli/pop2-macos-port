import sys, struct, re, collections
sys.path.insert(0, sys.argv[2])
import rsrc
r = rsrc.parse(open(sys.argv[1],'rb').read())
names = []
traps = collections.Counter()
for rid,(n,a,d) in sorted(r['CODE'].items()):
    if rid==0: continue
    for i in range(4, len(d)-4, 2):
        w = struct.unpack('>H', d[i:i+2])[0]
        if w in (0x4E75, 0x4ED0, 0x4E74):
            b = d[i+2]
            if b == 0x80:
                ln = d[i+3]; s = d[i+4:i+4+ln]
            elif 0x80 < b < 0xA0:
                ln = b - 0x80; s = d[i+3:i+3+ln]
            else: continue
            if len(s)==ln and re.fullmatch(rb'[A-Za-z_%.$][A-Za-z0-9_%.$:]*', s or b'!'):
                names.append((rid, i, s.decode()))
        if (w & 0xF000) == 0xA000: traps[w]+=1
print('macsbug names:', len(names))
for x in names[:40]: print(x)
print('distinct A-words', len(traps))

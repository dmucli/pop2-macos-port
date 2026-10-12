import struct, sys
def parse(data):
    doff, moff, dlen, mlen = struct.unpack('>IIII', data[:16])
    m = data[moff:moff+mlen]
    tlo, nlo = struct.unpack('>HH', m[24:28])
    ntypes = struct.unpack('>H', m[28:30])[0] + 1
    res = {}
    for i in range(ntypes):
        t, cnt, roff = struct.unpack('>4sHH', m[30+i*8:38+i*8])
        for j in range(cnt+1):
            e = m[tlo+roff+j*12: tlo+roff+j*12+12]
            rid, noff, attr_off, _ = struct.unpack('>hHII', e)
            attrs = attr_off >> 24; off = attr_off & 0xFFFFFF
            ln = struct.unpack('>I', data[doff+off:doff+off+4])[0]
            body = data[doff+off+4:doff+off+4+ln]
            name = None
            if noff != 0xFFFF:
                nl = m[nlo+noff]; name = m[nlo+noff+1:nlo+noff+1+nl].decode('mac_roman')
            res.setdefault(t.decode('mac_roman'), {})[rid] = (name, attrs, body)
    return res
if __name__ == '__main__':
    r = parse(open(sys.argv[1],'rb').read())
    for t in sorted(r):
        ids = r[t]
        tot = sum(len(v[2]) for v in ids.values())
        print(f"{t!r:8} n={len(ids):4} bytes={tot:8}  ids={sorted(ids)[:12]}{'...' if len(ids)>12 else ''}")

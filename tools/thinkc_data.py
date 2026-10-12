"""Decode a THINK C application's initialized globals (DATA 0 + ZERO 0) into the
A5-relative image, as done by its startup code (CODE 1, 0x100b4):
copy words from DATA; after each zero word, read a count from ZERO and clear
count+1 more bytes; stop when the destination reaches A5.
DREL 0 lists A5-relative offsets of pointers to relocate (bit 0 set: STRS-based)."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsrc

def decode(app_fork):
    r = rsrc.parse(open(app_fork, 'rb').read())
    code0 = r['CODE'][0][2]
    above, below = struct.unpack('>II', code0[:8])
    data = r['DATA'][0][2]; zero = r['ZERO'][0][2]
    out = bytearray(below)
    di = zi = oi = 0
    while oi < below:
        w = data[di:di + 2]; di += 2
        out[oi:oi + 2] = w; oi += 2
        if w == b'\x00\x00':
            cnt = struct.unpack('>H', zero[zi:zi + 2])[0]; zi += 2
            oi += cnt + 1          # bytes already zero
    drel = [struct.unpack('>H', r['DREL'][0][2][i:i + 2])[0] for i in range(0, len(r['DREL'][0][2]), 2)]
    return bytes(out[:below]), below, drel

if __name__ == '__main__':
    img, below, drel = decode(sys.argv[1])
    a5 = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
    print('below=%#x drel=%d entries' % (below, len(drel)))
    if len(sys.argv) > 3:
        off = int(sys.argv[3], 0)
        b = img[below + off: below + off + 64]
        print(b.hex(), b)

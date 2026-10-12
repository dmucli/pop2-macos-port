#!/usr/bin/env python3
"""Find Pascal-convention functions (callee pops its arguments) in the flat image.

Epilogues: `RTD #n`, or `movea.l (SP)+,A0/A1 ; addq/lea #n,SP ; jmp (A0/A1)`.
The function result lives in the caller-reserved slot at (8+n,A6); a write to it
gives the result size. Zero-argument functions with a write to (8,A6) are Pascal too.
Usage: detect_pascal.py <fn_index.json> <pop2_image.bin> <out.json> [skip_lo-skip_hi ...]
"""
import json, struct, sys

def scan(idx, img, skips):
    out = []
    for f in idx:
        a, e = f['entry'], f['end']
        if any(lo <= a < hi for lo, hi in skips):
            continue
        body = img[a:e]
        purge = None
        for i in range(0, len(body) - 1, 2):
            w = struct.unpack('>H', body[i:i + 2])[0]
            if w == 0x4E74 and i + 4 <= len(body):
                purge = struct.unpack('>H', body[i + 2:i + 4])[0]; break
            if w in (0x205F, 0x225F):
                n = None
                for k in range(i + 2, min(i + 14, len(body) - 1), 2):
                    x = struct.unpack('>H', body[k:k + 2])[0]
                    if x == 0x4FEF: n = struct.unpack('>H', body[k + 2:k + 4])[0]
                    elif (x & 0xF1FF) in (0x508F, 0x504F): n = ((x >> 9) & 7) or 8
                    elif x in (0x4ED0, 0x4ED1):
                        if n: purge = n
                        break
                if purge: break
        linked = len(body) >= 2 and struct.unpack('>H', body[:2])[0] == 0x4E56
        if purge is None and not (linked and not f['params']):
            continue
        tgt = 8 + (purge or 0)
        res = 0
        for i in range(0, len(body) - 3, 2):
            w = struct.unpack('>H', body[i:i + 2])[0]; d = struct.unpack('>h', body[i + 2:i + 4])[0]
            if d != tgt: continue
            if (w & 0xFFC0) == 0x3D40: res = max(res, 2)
            if (w & 0xFFC0) == 0x2D40: res = max(res, 4)
            if (w & 0xFFC0) == 0x1D40: res = max(res, 1)
        if purge is None and not res:
            continue
        out.append({'entry': a, 'name': f['name'], 'purge': purge or 0, 'ret': res})
    return out

if __name__ == '__main__':
    idx = json.load(open(sys.argv[1])); img = open(sys.argv[2], 'rb').read()
    skips = [tuple(int(x, 0) for x in s.split('-')) for s in sys.argv[4:]]
    out = scan(idx, img, skips)
    json.dump(out, open(sys.argv[3], 'w'))
    print(len(out), 'pascal functions')

"""Lay out a classic 68k Mac application (CODE resources) as a flat memory image
for Ghidra, with the jump table resolved, plus a JSON symbol/metadata file.

Memory map
  0x00000000  low memory globals (zero)
  0x00010000  CODE segments 1..N (each resource copied as-is incl. 4-byte header)
  A5_BASE     application globals (below A5) and jump table (above A5)
  0x00200000  trap stubs: line-A word w -> 0x00200000 + (w & 0xFFF) * 8
  0x00208000  dispatcher-variant stubs (selector-specific), 8 bytes each
"""
import sys, os, json, struct, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rsrc

SEG_BASE = 0x00010000
A5 = 0x00108000
TRAP_BASE = 0x00200000
VAR_BASE = 0x00208000
IMAGE_SIZE = 0x00210000

NAME_RE = re.compile(rb'[A-Za-z_%.$][A-Za-z0-9_%.$:]*')

def macsbug_scan(d):
    """Return list of (start, end, name) for functions in a near-model CODE segment body."""
    funcs = []
    pos = 4
    i = 4
    while i < len(d) - 4:
        w = struct.unpack('>H', d[i:i+2])[0]
        sym = None
        if w in (0x4E75, 0x4ED0) or (w == 0x4E74 and i + 4 < len(d)):
            j = i + (4 if w == 0x4E74 else 2)
            b = d[j]
            if b == 0x80:
                ln = d[j+1]; s = d[j+2:j+2+ln]; nend = j + 2 + ln
            elif 0x80 < b < 0xA0:
                ln = b - 0x80; s = d[j+1:j+1+ln]; nend = j + 1 + ln
            else:
                ln = -1; s = b''
            if ln > 0 and len(s) == ln and NAME_RE.fullmatch(s):
                sym = s.decode()
        if sym:
            funcs.append((pos, j, sym))
            if nend & 1: nend += 1
            lit = struct.unpack('>H', d[nend:nend+2])[0] if nend + 2 <= len(d) else 0
            if lit > 0x4000: lit = 0
            nxt = nend + 2 + lit
            if nxt & 1: nxt += 1
            pos = i = nxt
            continue
        i += 2
    return funcs, pos

def main(app_fork, traps_json, out_dir):
    r = rsrc.parse(open(app_fork, 'rb').read())
    traps = json.load(open(traps_json))
    img = bytearray(IMAGE_SIZE)
    meta = {'A5': A5, 'segments': {}, 'functions': [], 'jumptable': [], 'traps': [], 'variants': [],
            'lowmem': traps['lowmem']}
    code = r['CODE']
    above, below, jtsize, jtoff = struct.unpack('>IIII', code[0][2][:16])
    jt = code[0][2][16:16+jtsize]
    meta.update(above=above, below=below, jtsize=jtsize, jtoff=jtoff)
    # place segments
    addr = SEG_BASE
    for sid in sorted(code):
        if sid == 0: continue
        name, attrs, d = code[sid]
        img[addr:addr+len(d)] = d
        first_jt, n_jt = struct.unpack('>HH', d[:4])
        meta['segments'][sid] = {'name': name, 'base': addr, 'size': len(d), 'attrs': attrs,
                                 'first_jt': first_jt, 'n_jt': n_jt}
        if len(d) > 4:
            funcs, tail = macsbug_scan(d)
            for s, e, n in funcs:
                meta['functions'].append({'seg': sid, 'addr': addr + s, 'end': addr + e, 'name': n})
            meta['segments'][sid]['unnamed_tail'] = [addr + tail, addr + len(d)]
        addr = (addr + len(d) + 0x0F) & ~0x0F
    assert addr < A5 - below, hex(addr)
    # globals: A5 - below .. A5 ; app params A5..A5+0x20 ; jump table A5+jtoff
    jtaddr = A5 + jtoff
    for k in range(0, jtsize, 8):
        e = jt[k:k+8]
        off, op, seg, trap = struct.unpack('>HHHH', e)
        ent = jtaddr + k
        if op == 0x3F3C and trap == 0xA9F0 and seg in meta['segments']:
            target = meta['segments'][seg]['base'] + 4 + off
            img[ent:ent+8] = struct.pack('>HHI', seg, 0x4EF9, target)
            meta['jumptable'].append({'entry': ent + 2, 'a5off': jtoff + k + 2, 'seg': seg, 'target': target})
        else:
            img[ent:ent+8] = e
    # trap stubs
    tt = traps['traps']
    def lookup(w):
        if w >= 0xA800:
            return tt.get(f'{w & ~0x0400:04X}')
        for cand in (w, w & 0xF1FF, (w & 0xFF) | 0xA000, (w & 0xFF) | 0xA100, (w & 0xFF) | 0xA200, (w & 0xFF) | 0xA300):
            if f'{cand:04X}' in tt: return tt[f'{cand:04X}']
        return None
    EXTRA = {0xA9F0: ('LoadSeg', 2), 0xA9F1: ('UnloadSeg', 4), 0xA9F4: ('ExitToShell', 0),
             0xA9FF: ('Debugger', 0), 0xABFF: ('DebugStr', 4), 0xA9C9: ('SysError', 0)}
    for w in range(0xA000, 0xB000):
        info = lookup(w)
        stub = TRAP_BASE + (w & 0xFFF) * 8
        if w in EXTRA:
            n, pop = EXTRA[w]
            info = {'name': n, 'pop': pop, 'regbased': w == 0xA9C9, 'args': [], 'return': 'void', 'retsize': 0}
        if info is None:
            img[stub:stub+2] = b'\x4e\x75'
            meta['traps'].append({'word': w, 'addr': stub, 'name': f'_Trap{w:04X}', 'known': False})
            continue
        pop = info.get('pop') or 0
        if not info.get('regbased') and pop:
            img[stub:stub+4] = struct.pack('>HH', 0x4E74, pop)
        else:
            img[stub:stub+2] = b'\x4e\x75'
        meta['traps'].append({'word': w, 'addr': stub, 'name': '_' + info['name'], 'known': True,
                              'pop': pop, 'regbased': bool(info.get('regbased')),
                              'dispatcher': bool(info.get('dispatcher_root')), 'loc': info.get('loc'),
                              'args': info.get('args', []), 'return': info.get('return'),
                              'retsize': info.get('retsize', 0), 'returnreg': info.get('returnreg')})
    for k, v in enumerate(traps['variants']):
        stub = VAR_BASE + k * 8
        pop = v['pop']
        img[stub:stub+4] = struct.pack('>HH', 0x4E74, pop) if pop else b'\x4e\x75\x00\x00'
        meta['variants'].append({'addr': stub, 'name': '_' + v['name'], 'trap': v['trap'],
                                 'selector': v['selector'], 'loc': v['loc'], 'pop': pop,
                                 'args': v['args'], 'return': v['return'], 'retsize': v['retsize'],
                                 'dispatcher': v['dispatcher']})
    os.makedirs(out_dir, exist_ok=True)
    open(os.path.join(out_dir, 'pop2_image.bin'), 'wb').write(img)
    json.dump(meta, open(os.path.join(out_dir, 'pop2_meta.json'), 'w'), indent=1)
    nf = len(meta['functions'])
    print(f'segments={len(meta["segments"])} functions={nf} jt={len(meta["jumptable"])} '
          f'A5={A5:#x} globals={A5-below:#x}..{A5:#x} seg_end={addr:#x}')

if __name__ == '__main__':
    main(*sys.argv[1:4])

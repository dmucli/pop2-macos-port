#!/usr/bin/env python3
"""Build tools/fn_index.json from the Ghidra exports.

  build_index.py <work dir>

Reads (in <work dir>):
  ghidra_in/pop2_meta.json   segments + jump table (from build_image.py)
  functions.json             function list (FixNames.java)
  decomp_protos.json         decompiler prototypes (DecompProtos script)
  pascal_all.json            Pascal-convention info (detect_pascal.py), optional
  lib_map.json               THINK C library annotations, optional
"""
import json, os, sys

def main(work):
    meta = json.load(open(os.path.join(work, 'ghidra_in', 'pop2_meta.json')))
    fns = json.load(open(os.path.join(work, 'functions.json')))
    protos = {d['entry']: d for d in json.load(open(os.path.join(work, 'decomp_protos.json')))}
    pas_path = os.path.join(work, 'pascal_all.json')
    pascal = {p['entry']: p for p in json.load(open(pas_path))} if os.path.exists(pas_path) else {}
    lib_path = os.path.join(work, 'lib_map.json')
    lib = {l['entry']: l for l in json.load(open(lib_path))} if os.path.exists(lib_path) else {}
    segs = {int(k): v for k, v in meta['segments'].items()}
    jt = {j['target']: j['a5off'] for j in meta['jumptable']}
    out, names = [], {}
    for f in fns:
        e = f['entry']
        seg = next((k for k, v in segs.items() if v['base'] <= e < v['base'] + v['size']), None)
        if seg is None:
            continue
        d = protos.get(e, {})
        params = []
        for p in d.get('params', []):
            if p['stack'] < 0:
                params.append({'reg': p['reg'], 'size': p['size']})
                continue
            off, size = p['stack'], p['size']
            if size == 1 and off % 2 == 1:
                off -= 1
            size = 2 if size <= 2 else 4
            params.append({'off': off, 'size': size})
        ret = 0 if d.get('retname') == 'void' else d.get('ret', 4)
        if ret == 8:
            ret = 4
        nm = f['name']
        if nm in names:
            names[nm] += 1
            nm = '%s_%d' % (nm, names[nm])
        else:
            names[nm] = 0
        rec = {'name': nm, 'entry': e, 'end': f['end'], 'seg': seg, 'segoff': e - segs[seg]['base'],
               'jt': jt.get(e), 'named': f.get('named', False), 'params': params, 'ret': ret}
        if e in pascal:
            rec['conv'] = 'pascal'
            rec['purge'] = pascal[e]['purge']
            rec['ret'] = pascal[e]['ret']
            rec['retslot'] = rec['ret'] > 0
        if e in lib:
            l = lib[e]
            if l.get('conv') == 'pascal':
                rec['conv'] = 'pascal'
                if l.get('purge') is not None:
                    rec['purge'] = l['purge']
                rec['ret'] = l.get('ret', rec['ret'])
                rec['retslot'] = l.get('ret_where') == 'stack' and rec['ret'] > 0
            elif l.get('conv') == 'regs':
                rec['conv'] = 'regs'
        out.append(rec)
    json.dump(out, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'fn_index.json'), 'w'), indent=0)
    print('%d functions, %d pascal' % (len(out), sum(1 for o in out if o.get('conv') == 'pascal')))

if __name__ == '__main__':
    main(sys.argv[1])

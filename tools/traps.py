"""Build a Mac Toolbox trap table (name, stack bytes popped, register args) from
Multiversal Interfaces YAML definitions (https://github.com/autc04/multiversal)."""
import sys, os, json, re, yaml

PRIM = {
    'char': 1, 'unsigned char': 1, 'signed char': 1, 'uint8_t': 1, 'int8_t': 1,
    'short': 2, 'unsigned short': 2, 'int16_t': 2, 'uint16_t': 2,
    'int': 4, 'long': 4, 'unsigned long': 4, 'unsigned int': 4, 'unsigned': 4,
    'int32_t': 4, 'uint32_t': 4, 'float': 4, 'bool': 1, 'void': 0,
    'double': 8, 'Extended': 10, 'extended80': 10, 'Fixed': 4, 'Fract': 4,
    'INTEGER': 2, 'LONGINT': 4, 'Boolean': 1, 'BOOLEAN': 1, 'Byte': 1,
    'SignedByte': 1, 'CharParameter': 2, 'ProcPtr': 4, 'Ptr': 4, 'Handle': 4,
    'OSType': 4, 'ResType': 4, 'Size': 4, 'OSErr': 2, 'Point': 4, 'Str255': 256,
}

def load(defs_dir):
    entries = []
    for fn in sorted(os.listdir(defs_dir)):
        if fn.endswith('.yaml'):
            entries += [(fn, e) for e in (yaml.safe_load(open(os.path.join(defs_dir, fn))) or [])]
    return entries

def build(defs_dir):
    entries = load(defs_dir)
    sizes = dict(PRIM)
    for fn, e in entries:
        for kind in ('struct', 'union'):
            if kind in e and e[kind].get('size') is not None:
                sizes[e[kind]['name']] = e[kind]['size']
            elif kind in e:
                sizes.setdefault(e[kind]['name'], 99)  # unknown but large
        if 'enum' in e and e['enum'].get('name'):
            sizes[e['enum']['name']] = 2 if e['enum'].get('value_type', 'INTEGER') in ('INTEGER', 'short') else 4
        if 'funptr' in e:
            sizes[e['funptr']['name']] = 4
    typedefs = {e['typedef']['name']: e['typedef']['type'] for fn, e in entries if 'typedef' in e}

    def size_of(t, depth=0):
        t = t.strip()
        if t.endswith('*') or t.endswith(']') or t.startswith('const ') and t.endswith('*'):
            return 4
        t = re.sub(r'^(const|volatile)\s+', '', t)
        if t in sizes: return sizes[t]
        if t in typedefs and depth < 20: return size_of(typedefs[t], depth + 1)
        if t.endswith('Ptr') or t.endswith('Handle') or t.endswith('Proc') or t.endswith('UPP'):
            return 4
        return None

    def stack_size(t):
        s = size_of(t)
        if s is None: return 4, True
        if s <= 2: return 2, False
        if s <= 4: return 4, False
        return 4, False  # larger records passed by reference in Pascal

    dispatchers = {}
    for fn, e in entries:
        if 'dispatcher' in e:
            d = e['dispatcher']
            dispatchers[d['name']] = {'trap': d['trap'], 'loc': d.get('selector-location'), 'file': fn}

    traps = {}   # trapword -> info
    variants = []  # dispatcher variants
    for fn, e in entries:
        if 'function' not in e: continue
        f = e['function']
        args = f.get('args') or []
        regs = [a for a in args if 'register' in a]
        stack_args = [a for a in args if 'register' not in a]
        ret = f.get('return', 'void')
        info = {
            'name': f['name'], 'file': fn, 'return': ret,
            'returnreg': f.get('returnreg'),
            'args': [{'name': a.get('name') or f'arg{i}', 'type': a['type'],
                      'register': a.get('register'),
                      'stack': None if 'register' in a else stack_size(a['type'])[0]}
                     for i, a in enumerate(args)],
        }
        pop = sum(stack_size(a['type'])[0] for a in stack_args)
        info['pop'] = pop
        rs = size_of(ret) if ret != 'void' else 0
        info['retsize'] = 0 if not rs else (2 if rs <= 2 else 4)
        if 'trap' in f:
            tw = f['trap']
            regbased = bool(regs) or f.get('returnreg') or (tw < 0xA800)
            info['regbased'] = bool(regbased)
            if regbased and not stack_args: info['pop'] = 0
            info['variants'] = f.get('variants')
            traps.setdefault(tw, info)
        elif 'dispatcher' in f:
            d = dispatchers.get(f['dispatcher'])
            if not d: continue
            info['dispatcher'] = f['dispatcher']
            info['trap'] = d['trap']
            info['selector'] = f.get('selector')
            info['loc'] = d['loc']
            if d['loc'] == 'StackW': info['pop'] += 2
            if d['loc'] == 'StackL': info['pop'] += 4
            variants.append(info)
    for name, d in dispatchers.items():
        traps.setdefault(d['trap'], {'name': name, 'file': d['file'], 'args': [], 'pop': None,
                                     'regbased': False, 'dispatcher_root': True, 'loc': d['loc'],
                                     'return': 'void', 'retsize': 0, 'returnreg': None})
        traps[d['trap']]['dispatcher_root'] = True
        traps[d['trap']]['loc'] = d['loc']
    lowmem = []
    for fn, e in entries:
        if 'lowmem' in e:
            l = e['lowmem']
            lowmem.append({'name': l['name'], 'address': l['address'], 'type': l.get('type'),
                           'size': size_of(l.get('type', 'long')) or 4})
    return {'traps': {f'{k:04X}': v for k, v in sorted(traps.items())}, 'variants': variants,
            'lowmem': lowmem}

if __name__ == '__main__':
    out = build(sys.argv[1])
    json.dump(out, open(sys.argv[2], 'w'), indent=1)
    print(len(out['traps']), 'traps', len(out['variants']), 'dispatch variants', len(out['lowmem']), 'lowmem')

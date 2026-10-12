import sys, os, machfs
v = machfs.Volume()
v.read(open(sys.argv[1],'rb').read())
out = sys.argv[2]
def walk(folder, path):
    for name, item in folder.items():
        p = path + [name.replace('/', ':')]
        if isinstance(item, machfs.Folder):
            walk(item, p)
        else:
            d = os.path.join(out, *p[:-1]); os.makedirs(d, exist_ok=True)
            base = os.path.join(d, p[-1])
            if item.data: open(base, 'wb').write(item.data)
            if item.rsrc: open(base + '.rsrc_fork', 'wb').write(item.rsrc)
            open(base + '.finf', 'w').write(f"{item.type.decode('mac_roman')} {item.creator.decode('mac_roman')}\n")
walk(v, [])

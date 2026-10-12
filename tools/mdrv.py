"""Decrypt + decompress the SoundMusicSys 'MDRV' driver code resource
(reimplements PoP2's DecryptHandle / DecompressHandle / doLZSSDecode)."""
import sys, struct, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

def decrypt(data, key=0xDCE5, start=7):
    out = bytearray(data)
    for i in range(start, len(out)):
        b = out[i]
        out[i] = b ^ ((key >> 8) & 0xFF)
        key = (((b + key) & 0xFFFF) * 0xCE6D + 0x58BF) & 0xFFFF
    return bytes(out)

def lzss_decode(src, outlen):
    out = bytearray()
    i = 0; n = len(src)
    while i < n:
        flags = src[i]; i += 1
        for bit in range(8):
            if flags & 1:
                if i >= n or len(out) >= outlen: return bytes(out)
                out.append(src[i]); i += 1
            else:
                if i + 2 > n: return bytes(out)
                w = (src[i] << 8) | src[i + 1]; i += 2
                cnt = (w >> 12) + 3
                pos = len(out) + (w & 0xFFF) - 0x1000
                for _ in range(cnt):
                    if len(out) >= outlen: return bytes(out)
                    out.append(out[pos] if pos >= 0 else 0); pos += 1
            flags >>= 1
    return bytes(out)

def decompress_handle(data):
    size = struct.unpack('>I', data[:4])[0]
    return lzss_decode(data[4:], size), size

if __name__ == '__main__':
    import rsrc
    r = rsrc.parse(open(sys.argv[1], 'rb').read())
    name, attrs, d = r['MDRV'][11]
    dec = decrypt(d)
    out, size = decompress_handle(dec)
    print('expected', size, 'got', len(out))
    open(sys.argv[2], 'wb').write(out)

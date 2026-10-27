# texkey.py: the port's name for a texture in textures\dumps and textures\replacement (gpu/texturedumper.cpp Key, with
# gpu/shaders.cpp Hash): WxH_<key>_<format>.png, the key a hash of the texture's level 0 bytes seeded with its NV2A
# format, width and height.
import struct

M = 0xFFFFFFFFFFFFFFFF
K0, K1 = 0x9E3779B97F4A7C15, 0xE7037ED1A0B428DB


def _mix(a, b):
    r = (a & M) * (b & M)
    return (r & M) ^ (r >> 64)


def port_hash(b, h=1469598103934665603):
    seed = h ^ K0
    n = len(b)
    i = 0
    while i + 16 <= n:
        x, y = struct.unpack_from('<QQ', b, i)
        seed = _mix(x ^ K1, y ^ seed)
        i += 16
    r = n - i
    if r:
        if r >= 8:
            x = struct.unpack_from('<Q', b, i)[0]
            y = struct.unpack_from('<Q', b, n - 8)[0]
        else:
            x = int.from_bytes(b[i:n] + bytes(8 - r), 'little')
            y = 0
        seed = _mix(x ^ K1, y ^ seed ^ r)
    return _mix(seed ^ K0, n ^ K1)


NV2A = {b'DXT1': (0x0C, 8), b'DXT3': (0x0E, 16), b'DXT5': (0x0F, 16)}


def dds_name(path, ext='png'):
    d = open(path, 'rb').read()
    h, w = struct.unpack_from('<II', d, 12)
    cc = d[84:88]
    code, block = NV2A[cc]
    size = max(1, w // 4) * max(1, h // 4) * block
    key = port_hash(d[128:128 + size], port_hash(struct.pack('<3I', code, w, h))) or 1
    return '%ux%u_%016x_%s.%s' % (w, h, key, cc.decode(), ext)

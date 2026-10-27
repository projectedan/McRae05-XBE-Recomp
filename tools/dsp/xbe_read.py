import sys, struct
d = open(sys.argv[1], 'rb').read()
base = struct.unpack_from('<I', d, 0x104)[0]
nsec, sec_hdr = struct.unpack_from('<II', d, 0x11C)
va = int(sys.argv[2], 16); n = int(sys.argv[3], 16)
for i in range(nsec):
    o = sec_hdr - base + i * 56
    flags, vaddr, vsize, raddr, rsize, name = struct.unpack_from('<IIIIII', d, o)
    nm = d[name - base: d.index(b'\0', name - base)].decode()
    if vaddr <= va < vaddr + vsize:
        off = raddr + va - vaddr
        print('section', nm, hex(vaddr), 'file', hex(off), file=sys.stderr)
        open(sys.argv[4], 'wb').write(d[off: off + n])

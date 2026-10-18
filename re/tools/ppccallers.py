import sys, struct
img, base, target = sys.argv[1], int(sys.argv[2],16), int(sys.argv[3],16)
d = open(img,'rb').read()
for i in range(0, len(d)-3, 4):
    w = struct.unpack_from('>I', d, i)[0]
    if (w & 0xFC000003) == 0x48000001:
        off = w & 0x03FFFFFC
        if off & 0x02000000: off -= 0x04000000
        if base + i + off == target: print(hex(base+i))

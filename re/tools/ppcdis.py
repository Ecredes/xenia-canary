import sys, struct, capstone
img_path, base = sys.argv[1], int(sys.argv[2], 16)
data = open(img_path, 'rb').read()
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
md.skipdata = True
def dis(addr, n_before=12, n_after=24):
    start = addr - 4*n_before
    for ins in md.disasm(data[start-base:start-base+4*(n_before+n_after)], start):
        mark = '>' if ins.address == addr else ' '
        print(f"{mark}{ins.address:08X}  {ins.mnemonic:8} {ins.op_str}")
if sys.argv[3] == 'find':
    word = int(sys.argv[4], 16)
    hits = [base+i for i in range(0, len(data)-3, 4) if struct.unpack_from('>I', data, i)[0] == word]
    print('hits', [hex(h) for h in hits])
    for h in hits: print('----'); dis(h)
else:
    dis(int(sys.argv[3], 16), int(sys.argv[4]) if len(sys.argv) > 4 else 12, int(sys.argv[5]) if len(sys.argv) > 5 else 24)

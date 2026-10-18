"""Minimal XDVDFS (Xbox 360 game disc) reader: list or extract files.

Usage: python xdvdfs.py <iso> list
       python xdvdfs.py <iso> get <path-in-disc> <out-file>
"""

import struct
import sys

SECTOR = 2048
PARTITIONS = (0, 0xFD90000, 0x2080000, 0x18300000)


def find_base(f):
    for base in PARTITIONS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == b"MICROSOFT*XBOX*MEDIA":
            return base
    raise SystemExit("no XDVDFS volume descriptor found")


def walk(f, base, sector, size, prefix=""):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attr, name_len = struct.unpack_from(
            "<HHIIBB", data, off)
        if left == 0xFFFF:
            continue
        name = data[off + 14:off + 14 + name_len].decode("latin-1")
        path = f"{prefix}/{name}"
        if attr & 0x10:
            yield from walk(f, base, start, length, path)
        else:
            yield path, start, length
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    with open(iso, "rb") as f:
        base = find_base(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        files = {p.lower(): (p, s, n) for p, s, n in walk(f, base, root_sector,
                                                          root_size)}
        if cmd == "list":
            for p, s, n in sorted(files.values()):
                print(f"{n:12d} {p}")
            return
        want = sys.argv[3].lower()
        if not want.startswith("/"):
            want = "/" + want
        p, s, n = files[want]
        f.seek(base + s * SECTOR)
        with open(sys.argv[4], "wb") as out:
            out.write(f.read(n))
        print(f"wrote {n} bytes of {p}")


main()

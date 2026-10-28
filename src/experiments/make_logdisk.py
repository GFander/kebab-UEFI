#!/usr/bin/env python3
"""Build the FAT16 disk image used by LogExportDxe.

Usage: python3 make_logdisk.py <output.img>
Prints the byte offset of BOOTLOG.TXT data; it must match LOG_FILE_OFFSET
(0xC200) in LogExportDxe.c.
"""
import struct, subprocess, sys, os

OUT = sys.argv[1] if len(sys.argv) > 1 else 'LogDisk.img'
SIZE_MB = 4
FILE_SIZE = 512 * 1024

with open(OUT, 'wb') as f:
    f.write(b'\0' * (SIZE_MB << 20))
subprocess.run(['mkfs.vfat', '-F', '16', '-s', '1', '-n', 'UEFILOG', OUT],
               check=True, stdout=subprocess.DEVNULL)

d = bytearray(open(OUT, 'rb').read())
bs = d[0:512]
bps, spc, rsvd, nfats, rootent, totsec16, media, fatsz16 = struct.unpack_from('<HBHBHHBH', bs, 11)
totsec = totsec16 or struct.unpack_from('<I', bs, 32)[0]
root_sec = rsvd + nfats * fatsz16
root_sectors = (rootent * 32 + bps - 1) // bps
data_sec = root_sec + root_sectors
cluster = bps * spc

print(f'bps={bps} spc={spc} rsvd={rsvd} nfats={nfats} rootent={rootent} fatsz={fatsz16}')
print(f'root_sec={root_sec} data_sec={data_sec} cluster={cluster}B')

nclust = (FILE_SIZE + cluster - 1) // cluster
FIRST = 2
fat_off = rsvd * bps
for f in range(nfats):
    base = fat_off + f * fatsz16 * bps
    for i in range(nclust):
        c = FIRST + i
        nxt = c + 1 if i < nclust - 1 else 0xFFFF
        struct.pack_into('<H', d, base + c * 2, nxt)
    struct.pack_into('<H', d, base, 0xFFF8)
    struct.pack_into('<H', d, base + 2, 0xFFFF)

ent = bytearray(32)
ent[0:11] = b'BOOTLOG TXT'
ent[11] = 0x20
struct.pack_into('<H', ent, 26, FIRST)
struct.pack_into('<I', ent, 28, FILE_SIZE)
d[root_sec * bps: root_sec * bps + 32] = ent

file_off = data_sec * bps
d[file_off:file_off + FILE_SIZE] = b'\n' * FILE_SIZE
open(OUT, 'wb').write(d)

print(f'FILE DATA OFFSET = 0x{file_off:X} ({file_off})')
print(f'capacity = {FILE_SIZE} bytes')
if file_off != 0xC200:
    print('WARNING: offset differs from LOG_FILE_OFFSET in LogExportDxe.c')
    sys.exit(1)

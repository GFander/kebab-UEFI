import struct, sys, os
SIZE = 512*1024          # 512 KiB
SEC  = 512
TOT  = SIZE // SEC
RESV = 1
NFATS= 2
ROOT = 512 // 32          # 512 root entries
SPC  = 1                  # 1 sector per cluster
FATSEC = 3                # FAT12: 3 sectors = 1024 entries
data_start = RESV + NFATS*FATSEC + ROOT//(SEC//32)
clusters = TOT - data_start
assert clusters < 4085, clusters
img = bytearray(SIZE)
bs = bytearray(512)
bs[0:3] = b'\xEB\x3C\x90'
bs[3:11] = b'MSDOS5.0'
struct.pack_into('<H', bs, 11, SEC)
bs[13] = SPC
struct.pack_into('<H', bs, 14, RESV)
bs[16] = NFATS
struct.pack_into('<H', bs, 17, ROOT)
struct.pack_into('<H', bs, 19, TOT if TOT < 65536 else 0)
bs[21] = 0xF8
struct.pack_into('<H', bs, 22, FATSEC)
struct.pack_into('<H', bs, 24, 32)      # sectors/track
struct.pack_into('<H', bs, 26, 64)      # heads
struct.pack_into('<H', bs, 28, 0)       # hidden
struct.pack_into('<I', bs, 32, 0)       # total32 (unused)
bs[36] = 0x80
bs[38] = 0x29
struct.pack_into('<I', bs, 39, 0x12345678)
bs[43:54] = b'NO NAME    '
bs[54:62] = b'FAT12   '
bs[510] = 0x55; bs[511] = 0xAA
img[0:512] = bs
# FAT12 entries: 0=FF8, 1=FFF
fat = bytearray(FATSEC*SEC)
fat[0:3] = bytes([0xF8,0xFF,0xFF])
fat[3] = 0xFF           # cluster 2 -> 0xFFF (end of chain)
fat[4] = 0x0F
# copy FATs
for i in range(NFATS):
    off = (RESV + i*FATSEC)*SEC
    img[off:off+len(fat)] = fat
# root dir
root_off = (RESV + NFATS*FATSEC)*SEC
data = open('/tmp/xnu/XNU-TEST.TXT','rb').read()
assert len(data) <= SEC*SPC
cl = 2
e = bytearray(32)
name, ext = b'XNU-TEST', b'TXT'
e[0:8] = name.ljust(8)
e[8:11] = ext.ljust(3)
e[11] = 0x20
struct.pack_into('<H', e, 26, cl)
struct.pack_into('<I', e, 28, len(data))
img[root_off:root_off+32] = e
d_off = (data_start + (cl-2)*SPC)*SEC
img[d_off:d_off+len(data)] = data
open('/tmp/xnu/xnu-test-fat12.img','wb').write(img)
print(f'  built {SIZE} bytes, data_start_lba={data_start}, clusters={clusters}, file_lba={d_off//SEC}')

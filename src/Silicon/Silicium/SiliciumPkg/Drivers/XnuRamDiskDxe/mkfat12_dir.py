#!/usr/bin/env python3
"""Build a FAT12 image with subdirectories and long file names (XNU-003).

Two passes: allocate every directory and file cluster first, then emit the
directory entries and the file payloads.  Simple enough to verify by eye.
"""

import struct
import sys

SECTOR = 512
SPC = 4
CLUSTER = SECTOR * SPC
RESERVED = 1
NUM_FATS = 2
ROOT_ENTRIES = 224
TOTAL_SECTORS = 1024
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
DATA_START = RESERVED + NUM_FATS + ROOT_SECTORS
DATA_CLUSTERS = (TOTAL_SECTORS - DATA_START) // SPC

ATTR_DIR = 0x10
ATTR_ARC = 0x20


def short_name(name):
    up = name.upper()
    base, _, ext = up.rpartition(".")
    if not base:
        base, ext = up, ""
    ok = 1 <= len(base) <= 8 and len(ext) <= 3 and all(
        c.isalnum() or c in "$%'-_@~`!(){}^#&" for c in base + ext)
    if ok:
        return (base.ljust(8) + ext.ljust(3)).encode("ascii"), False
    stem = "".join(c for c in base if c.isalnum())[:6] or "FILE"
    return (f"{stem}~1".ljust(8) + ext[:3].ljust(3)).encode("ascii"), True


def lfn_checksum(short):
    s = 0
    for b in short:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def lfn_slots(name, short):
    raw = name.encode("utf-16-le")
    units = [raw[i:i + 2] for i in range(0, len(raw), 2)]
    chunks = [units[i:i + 13] for i in range(0, len(units), 13)]
    if len(chunks[-1]) < 13:
        chunks[-1] = chunks[-1] + [b"\x00\x00"] + [b"\xff\xff"] * (13 - len(chunks[-1]) - 1)
    csum = lfn_checksum(short)
    out = []
    for idx in range(len(chunks) - 1, -1, -1):
        blob = b"".join(chunks[idx])
        e = bytearray(32)
        e[0] = (idx + 1) | (0x40 if idx == len(chunks) - 1 else 0)
        e[1:11] = blob[0:10]
        e[11] = 0x0F
        e[13] = csum
        e[14:26] = blob[10:22]
        e[28:32] = blob[22:26]
        out.append(bytes(e))
    return out


def entry(short, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = short
    e[11] = attr
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def build(tree, out_path):
    # ---- collect ----
    dirs = ["/"]
    files = []          # (path, data)
    order = []          # directory listing order: (dirpath, [child names])

    def walk(node, prefix):
        names = []
        for name, val in node.items():
            p = f"{prefix}/{name}" if prefix != "/" else f"/{name}"
            names.append((name, p, isinstance(val, dict)))
            if isinstance(val, dict):
                dirs.append(p)
                walk(val, p)
            else:
                files.append((p, val))
        order.append((prefix, names))

    walk(tree, "/")

    # ---- allocate ----
    nxt = 2
    dir_cluster = {}
    for d in dirs:
        dir_cluster[d] = nxt
        nxt += 1
    file_cluster = {}
    fat = [0] * (DATA_CLUSTERS + 2)
    fat[0], fat[1] = 0xFF0, 0xFFF
    # Every directory occupies exactly one cluster and must be marked EOC,
    # otherwise FatPkg sees a free cluster and cannot read its contents.
    for d in dirs:
        fat[dir_cluster[d]] = 0xFFF
    for p, data in files:
        n = max(1, (len(data) + CLUSTER - 1) // CLUSTER)
        file_cluster[p] = nxt
        for i in range(n):
            fat[nxt + i] = nxt + i + 1 if i < n - 1 else 0xFFF
        nxt += n

    def parent_of(p):
        return p.rsplit("/", 1)[0] or "/"

    # ---- emit directories ----
    data = bytearray(DATA_CLUSTERS * CLUSTER)

    def put_dir(dirpath):
        slots = []
        for name, p, is_dir in next(n for n in order if n[0] == dirpath)[1]:
            short, needs = short_name(name)
            if needs:
                slots += lfn_slots(name, short)
            if is_dir:
                slots.append(entry(short, ATTR_DIR, dir_cluster[p], 0))
            else:
                payload = dict(files)[p]
                slots.append(entry(short, ATTR_ARC, file_cluster[p], len(payload)))
        slots.append(b"\x00" * 32)
        blob = b"".join(slots)
        assert len(blob) <= CLUSTER, f"{dirpath} too large"
        off = (dir_cluster[dirpath] - 2) * CLUSTER
        data[off:off + len(blob)] = blob

    for d in dirs:
        put_dir(d if d != "/" else "/")

    # root special-case: dirs list has "/" and children
    for p, payload in files:
        off = (file_cluster[p] - 2) * CLUSTER
        data[off:off + len(payload)] = payload

    # ---- assemble ----
    img = bytearray(TOTAL_SECTORS * SECTOR)
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xEB\x3C\x90"
    bs[3:11] = b"MSWIN4.1"
    struct.pack_into("<H", bs, 11, SECTOR)
    bs[13] = SPC
    struct.pack_into("<H", bs, 14, RESERVED)
    bs[16] = NUM_FATS
    struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
    struct.pack_into("<H", bs, 22, 1)   # FATSz16: one sector per FAT
    struct.pack_into("<H", bs, 19, TOTAL_SECTORS)
    bs[21] = 0xF8
    struct.pack_into("<H", bs, 24, 1)
    struct.pack_into("<H", bs, 26, 1)
    bs[36] = 0x80
    bs[38] = 0x29
    struct.pack_into("<I", bs, 39, 0x584E5503)
    bs[43:54] = b"UNIXNU     "
    bs[54:62] = b"FAT12   "
    bs[510:512] = b"\x55\xAA"
    img[0:SECTOR] = bs

    fatb = bytearray(SECTOR)
    for i in range(len(fat)):
        if i % 2 == 0:
            fatb[i * 3 // 2] = fat[i] & 0xFF
            fatb[i * 3 // 2 + 1] = (fatb[i * 3 // 2 + 1] & 0xF0) | ((fat[i] >> 8) & 0x0F)
        else:
            fatb[i * 3 // 2] = (fatb[i * 3 // 2] & 0x0F) | ((fat[i] & 0x0F) << 4)
            fatb[i * 3 // 2 + 1] = (fat[i] >> 4) & 0xFF
    img[SECTOR:2 * SECTOR] = fatb[:SECTOR]
    img[2 * SECTOR:3 * SECTOR] = fatb[:SECTOR]

    root = bytearray()
    for name, p, is_dir in next(n for n in order if n[0] == "/")[1]:
        short, needs = short_name(name)
        if needs:
            root += b"".join(lfn_slots(name, short))
        if is_dir:
            root += entry(short, ATTR_DIR, dir_cluster[p], 0)
        else:
            root += entry(short, ATTR_ARC, file_cluster[p], len(dict(files)[p]))
    root += b"\x00" * 32
    roff = (RESERVED + NUM_FATS) * SECTOR
    img[roff:roff + len(root)] = root

    doff = DATA_START * SECTOR
    img[doff:doff + len(data)] = data
    assert len(img) == TOTAL_SECTORS * SECTOR
    open(out_path, "wb").write(img)
    return len(img)


def selftest(path):
    img = open(path, "rb").read()
    rsv = struct.unpack_from("<H", img, 14)[0]
    nf = img[16]
    rootn = struct.unpack_from("<H", img, 17)[0]
    spc = img[13]
    doff = (rsv + nf + rootn * 32 // SECTOR) * SECTOR
    lines = []

    def walk(off, depth):
        lfn = []
        while True:
            e = img[off:off + 32]
            off += 32
            if len(e) < 32 or e[0] == 0:
                return
            if e[0] == 0xE5:
                continue
            if e[11] == 0x0F:
                lfn.append(e)
                continue
            base = e[0:8].decode("ascii", "replace").rstrip()
            ext = e[8:11].decode("ascii", "replace").rstrip()
            name = base + ("." + ext if ext else "")
            long = ""
            if lfn:
                units = []
                for x in reversed(lfn):
                    units += [x[1:11], x[14:26], x[28:32]]
                long = b"".join(units).decode("utf-16-le", "ignore").split("\x00")[0]
            cl = struct.unpack_from("<H", e, 26)[0]
            sz = struct.unpack_from("<I", e, 28)[0]
            lines.append("  " * depth + f"{long or name:22} attr=0x{e[11]:02x} cl={cl} size={sz}")
            lfn = []
            if e[11] & ATTR_DIR and cl >= 2:
                walk(doff + (cl - 2) * spc * SECTOR, depth + 1)

    walk((rsv + nf) * SECTOR, 0)
    return lines


if __name__ == "__main__":
    out = sys.argv[1]
    payload = open(sys.argv[2], "rb").read()
    blboot = None
    toml = b"""version = 1

[boot]
timeout = 30
default = "hello"

[[entry]]
id = "hello"
title = "Hello Payload"
type = "efi"
path = "Payloads/HELLO.EFI"
args = "pause"

[[entry]]
id = "reboot"
title = "Restart"
type = "action"
action = "reset"

[[entry]]
id = "poweroff"
title = "Power off"
type = "action"
action = "shutdown"

[[entry]]
id = "fw"
title = "Return to firmware"
type = "action"
action = "exit"
"""
    import os
    def rd(pth):
        return open(pth, "rb").read() if os.path.exists(pth) else b""

    tree = {
        # The demo configuration is deliberately absent: UniXNU stops at the
        # first configuration it finds, and the real one lives on the payload
        # volume in boot_b.
        "EFI": {"UniXNU": {}},
        "UFSPT.EFI": rd("Build/kebabPkg/DEBUG_CLANGPDB/AARCH64/MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe/DEBUG/UfsPassThruDxe.efi"),
        "SCSIBUS.EFI": rd("Build/kebabPkg/DEBUG_CLANGPDB/AARCH64/MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe/DEBUG/ScsiBus.efi"),
        "SCSIDISK.EFI": rd("Build/kebabPkg/DEBUG_CLANGPDB/AARCH64/MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe/DEBUG/ScsiDisk.efi"),
        "XNUPAY.EFI": rd("Build/kebabPkg/DEBUG_CLANGPDB/AARCH64/SiliciumPkg/Drivers/XnuPayloadBlockIoDxe/XnuPayloadBlockIoDxe/DEBUG/XnuPayloadBlockIoDxe.efi"),
        "XNU-TEST.TXT": (
            b"XNU-000 RAM FAT probe file.\n"
            b"Generated for the OnePlus 8T kebab UEFI bring-up.\n"
            b"This file is read-only test data.\n"
        ),
    }
    n = build(tree, out)
    print(f"wrote {out} ({n} bytes)")
    for line in selftest(out):
        print(line)

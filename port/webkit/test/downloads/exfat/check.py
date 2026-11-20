#!/usr/bin/env python3
# check.py: the layout ps5exfat.c promises, read back from an image
# (exfat-test.sh runs it next to fsck_exfat):
#   check.py <image> <cluster bytes> [label]
# Prints "OK ..." or "BAD <what>" and exits non-zero on BAD.
import struct, sys

path, cluster = sys.argv[1], int(sys.argv[2])
label = sys.argv[3] if len(sys.argv) > 3 else ""
bad = []
with open(path, "rb") as f:
    boot = f.read(512 * 24)
    f.seek(0, 2)
    size = f.tell()

    def at(offset, length):
        f.seek(offset)
        return f.read(length)

    if boot[3:11] != b"EXFAT   ":
        bad.append("no exFAT boot sector at 0 (partitioned?)")
    part, vol = struct.unpack_from("<QQ", boot, 64)
    fat_off, fat_len, heap, count, root, serial = struct.unpack_from("<IIIIII", boot, 80)
    rev, flags = struct.unpack_from("<HH", boot, 104)
    bps, spc, nfats, drive, pct = boot[108], boot[109], boot[110], boot[111], boot[112]
    if part != 0:
        bad.append("PartitionOffset %d" % part)
    if vol * 512 != size:
        bad.append("VolumeLength %d sectors, file %d bytes" % (vol, size))
    if size % (1 << 20):
        bad.append("size %d isn't a multiple of 1 MiB" % size)
    if bps != 9 or (1 << (bps + spc)) != cluster:
        bad.append("sector/cluster shifts %d/%d" % (bps, spc))
    if fat_off != 128 or nfats != 1 or rev != 0x100 or flags != 0:
        bad.append("FatOffset %d, NumberOfFats %d, revision %x, flags %x" % (fat_off, nfats, rev, flags))
    if (heap * 512) % max(cluster, 65536):
        bad.append("cluster heap at %d isn't aligned" % (heap * 512))
    if heap * 512 + count * cluster != size:
        bad.append("heap + clusters != size")
    if boot[:512 * 12] != boot[512 * 12:]:
        bad.append("backup boot region differs")
    # The root's system entries.
    root_entries = at(heap * 512 + (root - 2) * cluster, 96)
    if root_entries[0] != 0x83 or root_entries[32] != 0x81 or root_entries[64] != 0x82:
        bad.append("root starts %02x %02x %02x" % (root_entries[0], root_entries[32], root_entries[64]))
    got = root_entries[2:2 + 2 * root_entries[1]].decode("utf-16-le")
    if got != label:
        bad.append("label %r, wanted %r" % (got, label))
    up_sum, = struct.unpack_from("<I", root_entries, 68)
    up_len, = struct.unpack_from("<Q", root_entries, 88)
    if up_sum != 0xE619D30D or up_len != 5836:
        bad.append("up-case table %08x/%d, not the specification's" % (up_sum, up_len))
    # Every allocation is chained: the FAT has no free entry before the
    # first free cluster in the bitmap, and none after it.
    bm_first, = struct.unpack_from("<I", root_entries, 52)
    bitmap = at(heap * 512 + (bm_first - 2) * cluster, (count + 7) // 8)
    used = 0
    while used < count and bitmap[used // 8] >> (used % 8) & 1:
        used += 1
    if any(bitmap[(used + 7) // 8:]) or (used % 8 and bitmap[used // 8] >> (used % 8)):
        bad.append("allocated clusters aren't one run from cluster 2")
    fat = at(fat_off * 512, (count + 2) * 4)
    entries = struct.unpack("<%dI" % (count + 2), fat)
    if any(e == 0 for e in entries[2:2 + used]) or any(entries[2 + used:]):
        bad.append("the FAT doesn't chain exactly the allocated clusters")
    if pct != used * 100 // count:
        bad.append("PercentInUse %d, used %d of %d" % (pct, used, count))

if bad:
    print("BAD " + "; ".join(bad))
    sys.exit(1)
print("OK %d MiB, %d KiB clusters, heap at %d KiB, %d of %d clusters used, serial %08x"
      % (size >> 20, cluster >> 10, heap * 512 >> 10, used, count, serial))

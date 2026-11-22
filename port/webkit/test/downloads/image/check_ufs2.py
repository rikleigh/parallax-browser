#!/usr/bin/env python3
"""check_ufs2.py image source: a UFS2 image (ps5ufs2.c's .ffpkg) checked
independently of its writer, the way fsck_ffs would, then compared with the
folder it was made from.

Read from FreeBSD's sys/ufs/ffs/fs.h and sys/ufs/ufs/dinode.h, not from the
writer: the superblock and its geometry (and ShadowMountPlus's recommended
-b 65536 -f 65536 -m 0), every cylinder group (magic, index, block count,
the inode and block maps, the cluster map, the summary), the summary area
and the superblock's totals, every inode reachable from the root (mode,
link count, size, block count, block pointers inside the data area and
owned once), the directories (record lengths, "." and ".."), and each
file's bytes against the source. Also that every file is one contiguous run
(what the writer promises: ShadowMountPlus's fast path reads big blocks).

Prints "OK <summary>" or "BAD <what>" (exit 1).
"""
import os
import struct
import sys

SBLOCK = 65536
MAGIC = 0x19540119
CG_MAGIC = 0x00090255
IFMT, IFDIR, IFREG = 0o170000, 0o040000, 0o100000
ROOTINO = 2
NDADDR = 12


class Bad(Exception):
    pass


def u32(b, at):
    return struct.unpack_from('<I', b, at)[0]


def i32(b, at):
    return struct.unpack_from('<i', b, at)[0]


def u64(b, at):
    return struct.unpack_from('<Q', b, at)[0]


def main(image_path, source):
    f = open(image_path, 'rb')
    size_on_disk = os.fstat(f.fileno()).st_size

    def read(at, n):
        f.seek(at)
        b = f.read(n)
        if len(b) != n:
            raise Bad(f'short read at {at}')
        return b

    sb = read(SBLOCK, 8192)
    if u32(sb, 1372) != MAGIC:
        raise Bad('no UFS2 magic')
    bsize, fsize, frag = i32(sb, 48), i32(sb, 52), i32(sb, 56)
    if (bsize, fsize, frag) != (65536, 65536, 1):
        raise Bad(f'geometry {bsize}/{fsize}/{frag}, not 65536/65536/1')
    if i32(sb, 60) != 0:
        raise Bad(f'minfree {i32(sb, 60)}, not 0')
    sblkno, cblkno, iblkno, dblkno = (i32(sb, o) for o in (8, 12, 16, 20))
    ncg, ipg, fpg = i32(sb, 44), i32(sb, 184), i32(sb, 188)
    inopb, nindir = i32(sb, 120), i32(sb, 116)
    fs_size, fs_dsize, csaddr = u64(sb, 1080), u64(sb, 1088), u64(sb, 1096)
    cssize = i32(sb, 156)
    if inopb != bsize // 256 or nindir != bsize // 8:
        raise Bad('inopb/nindir')
    if not (sblkno < cblkno < iblkno < dblkno) or dblkno != iblkno + ipg // inopb:
        raise Bad(f'metadata blocks {sblkno} {cblkno} {iblkno} {dblkno}, ipg {ipg}')
    if not ((ncg - 1) * fpg < fs_size <= ncg * fpg):
        raise Bad(f'fs_size {fs_size} outside {ncg} groups of {fpg}')
    if fs_size * fsize != size_on_disk:
        raise Bad(f'the file is {size_on_disk} bytes, the filesystem {fs_size * fsize}')
    if csaddr != dblkno or cssize < ncg * 16:
        raise Bad('summary area')
    if u64(sb, 1000) != SBLOCK or u64(sb, 992) != SBLOCK:
        raise Bad('sblockloc')
    if i32(sb, 128) != 1 or sb[209] != 1:
        raise Bad('fs_optim / fs_clean')
    if read(SBLOCK - 20, 4) != struct.pack('<I', MAGIC):
        raise Bad('no fsrecovery block')
    csblocks = (cssize + bsize - 1) // bsize
    last = fs_size - (ncg - 1) * fpg
    if last <= dblkno:
        raise Bad(f'the last group ({last} blocks) holds no data')

    # owner[block] for every block the inodes use
    owner = {}
    meta = set()
    for c in range(ncg):
        base = c * fpg
        for b in range(base + sblkno, base + dblkno):
            meta.add(b)
        if c == 0:
            for b in range(0, sblkno):
                meta.add(b)
            for b in range(dblkno, dblkno + csblocks):
                meta.add(b)
        # backup superblocks: geometry the same as the primary
        bk = read((base + sblkno) * bsize, 8192)
        for o in (8, 12, 16, 20, 44, 48, 52, 184, 188, 1080, 1372):
            if bk[o:o + 4] != sb[o:o + 4]:
                raise Bad(f'group {c}: backup superblock differs at {o}')
        if u64(bk, 992) != (base + sblkno) * bsize:
            raise Bad(f'group {c}: backup superblock\'s own location')

    def inode(ino):
        if ino < ROOTINO or ino >= ncg * ipg:
            raise Bad(f'inode {ino} out of range')
        c, i = divmod(ino, ipg)
        blk = c * fpg + iblkno + i // inopb
        return read(blk * bsize + (i % inopb) * 256, 256)

    def claim(block, ino):
        if block <= 0 or block >= fs_size:
            raise Bad(f'inode {ino}: block {block} outside the volume')
        if block in meta:
            raise Bad(f'inode {ino}: block {block} is metadata')
        if block in owner:
            raise Bad(f'block {block} owned by inodes {owner[block]} and {ino}')
        owner[block] = ino

    def blocks_of(ino, d):
        """The data blocks of inode d in logical order; claims indirect blocks too."""
        size = u64(d, 16)
        n = (size + bsize - 1) // bsize
        out = []
        for i in range(min(n, NDADDR)):
            out.append(u64(d, 112 + 8 * i))
        for i in range(min(n, NDADDR), NDADDR):
            if u64(d, 112 + 8 * i):
                raise Bad(f'inode {ino}: a direct pointer past its size')
        left = n - min(n, NDADDR)
        indirect_count = 0

        def walk(block, level, want):
            nonlocal indirect_count
            if not block:
                raise Bad(f'inode {ino}: a hole in its block map')
            claim(block, ino)
            indirect_count += 1
            ptrs = struct.unpack_from(f'<{nindir}Q', read(block * bsize, bsize))
            per = nindir ** (level - 1)
            got = []
            used = (want + per - 1) // per
            for j in range(used):
                if level == 1:
                    got.append(ptrs[j])
                else:
                    got.extend(walk(ptrs[j], level - 1, min(per, want - j * per)))
            if any(ptrs[used:]):
                raise Bad(f'inode {ino}: indirect pointers past its size')
            return got

        for level in (1, 2, 3):
            root = u64(d, 208 + 8 * (level - 1))
            if not left:
                if root:
                    raise Bad(f'inode {ino}: an indirect root past its size')
                continue
            take = min(left, nindir ** level)
            out.extend(walk(root, level, take))
            left -= take
        for b in out:
            claim(b, ino)
        return out, indirect_count

    files = dirs = 0
    used_inodes = set()
    file_bytes = 0
    discontiguous = []

    def contiguous(blocks):
        # one run of the allocatable blocks: consecutive except across the
        # metadata at a group's start
        for a, b in zip(blocks, blocks[1:]):
            if b == a + 1:
                continue
            if b > a and all(x in meta or x % fpg < sblkno for x in range(a + 1, b)):
                continue
            return False
        return True

    def check_dir(ino, parent, src, depth):
        nonlocal files, dirs, file_bytes
        if depth > 128:
            raise Bad('nested too deep')
        d = inode(ino)
        if u32(d, 0) & 0xffff & IFMT != IFDIR:
            raise Bad(f'{src}: not a directory in the image')
        used_inodes.add(ino)
        dirs += 1
        size = u64(d, 16)
        if size % 512 or not size:
            raise Bad(f'{src}: directory size {size}')
        blocks, ind = blocks_of(ino, d)
        if u64(d, 24) != (len(blocks) + ind) * (bsize // 512):
            raise Bad(f'{src}: di_blocks')
        data = b''.join(read(b * bsize, bsize) for b in blocks)[:size]
        names = {}
        at = 0
        while at < size:
            chunk_end = (at // 512 + 1) * 512
            e_ino, reclen, typ, namlen = struct.unpack_from('<IHBB', data, at)
            if reclen < 8 or reclen % 4 or at + reclen > chunk_end or 8 + namlen > reclen:
                raise Bad(f'{src}: a bad entry at {at}')
            name = data[at + 8:at + 8 + namlen]
            if data[at + 8 + namlen] != 0:
                raise Bad(f'{src}: a name without its NUL')
            if e_ino:
                if name in names:
                    raise Bad(f'{src}: {name!r} twice')
                names[name] = (e_ino, typ)
            at += reclen
        if names.get(b'.', (0,))[0] != ino or names.get(b'..', (0,))[0] != parent:
            raise Bad(f'{src}: "." or ".." wrong')
        subdirs = 0
        listed = set(os.listdir(src))
        seen = set()
        for name, (e_ino, typ) in names.items():
            if name in (b'.', b'..'):
                continue
            text = os.fsdecode(name)
            seen.add(text)
            path = os.path.join(src, text)
            if text not in listed:
                raise Bad(f'{path} is in the image, not in the folder')
            if os.path.isdir(path) and not os.path.islink(path):
                if typ != 4:
                    raise Bad(f'{path}: entry type {typ}')
                subdirs += 1
                check_dir(e_ino, ino, path, depth + 1)
            else:
                if typ != 8:
                    raise Bad(f'{path}: entry type {typ}')
                fd = inode(e_ino)
                if u32(fd, 0) & 0xffff & IFMT != IFREG:
                    raise Bad(f'{path}: not a file in the image')
                if struct.unpack_from('<H', fd, 2)[0] != 1:
                    raise Bad(f'{path}: link count')
                if e_ino in used_inodes:
                    raise Bad(f'{path}: inode {e_ino} used twice')
                used_inodes.add(e_ino)
                fsz = u64(fd, 16)
                if fsz != os.path.getsize(path):
                    raise Bad(f'{path}: {fsz} bytes in the image')
                blocks, ind = blocks_of(e_ino, fd)
                if u64(fd, 24) != (len(blocks) + ind) * (bsize // 512):
                    raise Bad(f'{path}: di_blocks')
                if not contiguous(sorted(set(blocks) | {b for b, o in owner.items() if o == e_ino})):
                    discontiguous.append(path)
                with open(path, 'rb') as s:
                    for i, b in enumerate(blocks):
                        want = s.read(bsize)
                        got = read(b * bsize, len(want))
                        if got != want:
                            raise Bad(f'{path}: block {i} differs')
                files += 1
                file_bytes += fsz
        if seen != listed:
            raise Bad(f'{src}: in the folder, not the image: {sorted(listed - seen)[:5]}')
        nlink = struct.unpack_from('<H', d, 2)[0]
        if nlink != 2 + subdirs:
            raise Bad(f'{src}: link count {nlink}, {2 + subdirs} wanted')

    check_dir(ROOTINO, ROOTINO, source, 0)
    if discontiguous:
        raise Bad(f'not contiguous: {discontiguous[:3]}')

    # the cylinder groups against what the inodes use
    tot_dir = tot_bfree = tot_ifree = 0
    csum = read(csaddr * bsize, ncg * 16)
    for c in range(ncg):
        base = c * fpg
        cg = read((base + cblkno) * bsize, bsize)
        if u32(cg, 4) != CG_MAGIC or u32(cg, 12) != c:
            raise Bad(f'group {c}: header')
        ndblk = min(fpg, fs_size - base)
        if u32(cg, 20) != ndblk or u32(cg, 116) != ipg:
            raise Bad(f'group {c}: ndblk/niblk')
        iusedoff, freeoff, clusteroff = u32(cg, 92), u32(cg, 96), u32(cg, 108)

        def bit(off, i):
            return cg[off + i // 8] >> (i % 8) & 1

        ndir = nifree = 0
        for i in range(ipg):
            ino = c * ipg + i
            want = ino in used_inodes or ino < ROOTINO
            if bit(iusedoff, i) != want:
                raise Bad(f'group {c}: inode {ino} map bit {bit(iusedoff, i)}')
            if not want:
                nifree += 1
            elif ino in used_inodes and u32(inode(ino), 0) & 0xffff & IFMT == IFDIR:
                ndir += 1
        nbfree = 0
        for i in range(ndblk):
            b = base + i
            free = b not in meta and b not in owner
            if bit(freeoff, i) != free:
                raise Bad(f'group {c}: block {b} free bit {bit(freeoff, i)}, owner {owner.get(b)}')
            if bit(clusteroff, i) != free:
                raise Bad(f'group {c}: block {b} cluster bit')
            nbfree += free
        cs = struct.unpack_from('<III', cg, 24)
        if cs != (ndir, nbfree, nifree):
            raise Bad(f'group {c}: summary {cs}, counted {(ndir, nbfree, nifree)}')
        if struct.unpack_from('<III', csum, c * 16) != cs:
            raise Bad(f'group {c}: the summary area differs from the group')
        tot_dir, tot_bfree, tot_ifree = tot_dir + ndir, tot_bfree + nbfree, tot_ifree + nifree
    if (u64(sb, 1008), u64(sb, 1016), u64(sb, 1024)) != (tot_dir, tot_bfree, tot_ifree):
        raise Bad(f'superblock totals {(u64(sb, 1008), u64(sb, 1016), u64(sb, 1024))}, counted {(tot_dir, tot_bfree, tot_ifree)}')
    if tot_bfree < 1024:
        raise Bad(f'only {tot_bfree} free blocks (1024 kept for writable mounts)')
    if tot_ifree < 2048:
        raise Bad(f'only {tot_ifree} free inodes (2048 kept)')
    print(f'OK {files} files, {dirs} folders, {file_bytes} bytes; {ncg} groups of {fpg} blocks, {ipg} inodes each; '
          f'{tot_bfree} blocks and {tot_ifree} inodes free')


if __name__ == '__main__':
    try:
        main(sys.argv[1], sys.argv[2])
    except Bad as e:
        print(f'BAD {e}')
        sys.exit(1)

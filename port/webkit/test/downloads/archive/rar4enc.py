#!/usr/bin/env python3
"""Encrypts an unencrypted RAR4 archive (or volume set) the way RAR 3.x
does, for testing decryption: each file's data (across all its volumes) is
one AES-128-CBC stream padded to 16 bytes at its end, with FHD_PASSWORD and
a salt in each of its headers (version 2.9, which AES needs); --hp also
sets MHD_PASSWORD and encrypts every header after the main one (a salt,
then the header padded to 16 bytes). AES through openssl; the key with
hashlib's SHA-1, exact for passwords under 28 characters.
  rar4enc.py [--hp] [--password P] [--drop NAME]... out_dir volume...
(--drop leaves an entry out, a link say.)
"""
import argparse, hashlib, os, struct, subprocess, zlib

SIG = b"Rar!\x1a\x07\x00"
FILE_HEAD, MAIN_HEAD = 0x74, 0x73


def kdf(pw, salt):
    raw = pw.encode("utf-16-le") + salt
    assert len(raw) < 64, "long passwords need RAR's SHA-1 quirk"
    h = hashlib.sha1()
    iv = bytearray(16)
    for i in range(0x40000):
        h.update(raw)
        h.update(bytes([i & 255, (i >> 8) & 255, (i >> 16) & 255]))
        if i % 0x4000 == 0:
            iv[i // 0x4000] = h.copy().digest()[19]
    d = h.digest()
    return b"".join(d[4 * k:4 * k + 4][::-1] for k in range(4)), bytes(iv)


def aes(key, iv, data):
    if not data:
        return b""
    return subprocess.run(["openssl", "enc", "-aes-128-cbc", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
                          input=data, capture_output=True, check=True).stdout


class Block:
    pass


def parse(path):
    data = open(path, "rb").read()
    p = data.index(SIG) + 7
    blocks = []
    while p + 7 <= len(data):
        crc, typ, flags, size = struct.unpack_from("<HBHH", data, p)
        b = Block()
        b.type, b.flags, b.head = typ, flags, bytearray(data[p:p + size])
        add = 0
        if typ == FILE_HEAD or flags & 0x8000:
            add = struct.unpack_from("<I", data, p + 7)[0]
            if typ == FILE_HEAD and flags & 0x100:
                add |= struct.unpack_from("<I", data, p + 32)[0] << 32
        b.data = data[p + size:p + size + add]
        blocks.append(b)
        p += size + add
        if typ == 0x7B:
            break
    return blocks


def fix_crc(head):
    struct.pack_into("<H", head, 0, zlib.crc32(bytes(head[2:])) & 0xFFFF)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hp", action="store_true")
    ap.add_argument("--password", default="password")
    ap.add_argument("--drop", action="append", default=[])
    ap.add_argument("out")
    ap.add_argument("volumes", nargs="+")
    a = ap.parse_args()
    salt = os.urandom(8)
    key, iv = kdf(a.password, salt)
    vols = [parse(v) for v in a.volumes]
    drop = [d.encode() for d in a.drop]
    vols = [[b for b in blocks if not (b.type == FILE_HEAD and bytes(b.head[32 + (8 if b.flags & 0x100 else 0):]
             [:struct.unpack_from("<H", b.head, 26)[0]]) in drop)] for blocks in vols]

    groups, cur = [], None
    for blocks in vols:
        for b in blocks:
            if b.type != FILE_HEAD or (b.flags & 0xE0) == 0xE0:  # directories: no data
                continue
            if not (b.flags & 1) or cur is None:
                cur = []
                groups.append(cur)
            cur.append(b)
            if not (b.flags & 2):
                cur = None
    for g in groups:
        plain = b"".join(b.data for b in g)
        pad = (-len(plain)) % 16
        enc = aes(key, iv, plain + bytes(pad))
        at = 0
        for i, b in enumerate(g):
            last = i == len(g) - 1
            n = len(b.data) + (pad if last else 0)
            b.data = enc[at:at + n]
            at += n
            h = b.head
            nlen = struct.unpack_from("<H", h, 26)[0]
            name_at = 32 + (8 if b.flags & 0x100 else 0)
            assert not b.flags & 0x400
            h[name_at + nlen:name_at + nlen] = salt
            struct.pack_into("<H", h, 3, b.flags | 0x0404)
            struct.pack_into("<H", h, 5, len(h))
            struct.pack_into("<I", h, 7, n & 0xFFFFFFFF)
            if b.flags & 0x100:
                struct.pack_into("<I", h, 32, n >> 32)
            h[24] = max(h[24], 29)  # UNP_VER: AES needs RAR 2.9
            if not last:  # a split part's CRC is its packed data's
                struct.pack_into("<I", h, 16, zlib.crc32(b.data))
            fix_crc(h)

    os.makedirs(a.out, exist_ok=True)
    for path, blocks in zip(a.volumes, vols):
        out = bytearray(SIG)
        hp = False
        for b in blocks:
            h = b.head
            if b.type == MAIN_HEAD and a.hp:
                struct.pack_into("<H", h, 3, b.flags | 0x80)
                fix_crc(h)
                out += h
                hp = True
                continue
            if hp:
                out += salt + aes(key, iv, bytes(h) + bytes((-len(h)) % 16))
            else:
                out += h
            out += b.data
        open(os.path.join(a.out, os.path.basename(path)), "wb").write(out)


main()

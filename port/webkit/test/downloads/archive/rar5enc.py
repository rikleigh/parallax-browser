#!/usr/bin/env python3
"""Encrypts an unencrypted RAR5 archive (or volume set) the way RAR does,
for testing decryption: each file's data (across all its volumes) is one
AES-256-CBC stream padded to 16 bytes at its end, with an encryption record
in each of its headers; checksums become HMACs unless --no-mac; --hp also
encrypts the headers (an archive encryption header in each volume, then an
IV and the padded header for every block). AES through openssl.
  rar5enc.py [--hp] [--no-mac] [--no-check] [--lg2 N] [--password P]
             [--pad-random] out_dir volume...
"""
import argparse, hashlib, hmac, os, subprocess, zlib

SIG = b"Rar!\x1a\x07\x01\x00"


def vint(b, p):
    v = s = 0
    while True:
        c = b[p]
        p += 1
        v |= (c & 0x7F) << s
        s += 7
        if not c & 0x80:
            return v, p


def venc(v):
    out = bytearray()
    while True:
        c = v & 0x7F
        v >>= 7
        if v:
            out.append(c | 0x80)
        else:
            out.append(c)
            return bytes(out)


def aes_enc(key, iv, data):
    assert len(data) % 16 == 0
    if not data:
        return b""
    return subprocess.run(["openssl", "enc", "-aes-256-cbc", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
                          input=data, capture_output=True, check=True).stdout


def kdf(pw, salt, lg2):
    n = 1 << lg2
    key = hashlib.pbkdf2_hmac("sha256", pw, salt, n)
    v1 = hashlib.pbkdf2_hmac("sha256", pw, salt, n + 16)
    v2 = hashlib.pbkdf2_hmac("sha256", pw, salt, n + 32)
    check = bytearray(8)
    for i, c in enumerate(v2):
        check[i % 8] ^= c
    return key, v1, bytes(check)


def crc_mac(hashkey, crc):
    d = hmac.new(hashkey, crc.to_bytes(4, "little"), "sha256").digest()
    v = 0
    for i, c in enumerate(d):
        v ^= c << ((i & 3) * 8)
    return v


class Block:
    pass


def parse(path):
    data = open(path, "rb").read()
    assert data.startswith(SIG), path
    p = len(SIG)
    blocks = []
    while p < len(data):
        size, q = vint(data, p + 4)
        hdr = data[q:q + size]
        b = Block()
        b.type, r = vint(hdr, 0)
        b.flags, r = vint(hdr, r)
        extra = dsize = 0
        if b.flags & 1:
            extra, r = vint(hdr, r)
        if b.flags & 2:
            dsize, r = vint(hdr, r)
        b.fields = hdr[r:size - extra]
        b.extra = hdr[size - extra:size]
        b.data = data[q + size:q + size + dsize]
        blocks.append(b)
        p = q + size + dsize
        if b.type == 5:
            break
    return blocks


def crc_offset(fields):
    """Offset of the CRC32 in a file/service header's fields, or None."""
    ff, r = vint(fields, 0)
    _, r = vint(fields, r)  # unpacked size
    _, r = vint(fields, r)  # attributes
    if ff & 2:
        r += 4
    return r if ff & 4 else None


def build(b, extra, dsize):
    flags = b.flags & ~3
    if extra:
        flags |= 1
    if b.data or dsize or b.flags & 2:
        flags |= 2
    body = venc(b.type) + venc(flags)
    if extra:
        body += venc(len(extra))
    if flags & 2:
        body += venc(dsize)
    body += b.fields + extra
    head = venc(len(body)) + body
    return zlib.crc32(head).to_bytes(4, "little") + head


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hp", action="store_true")
    ap.add_argument("--no-mac", action="store_true")
    ap.add_argument("--no-check", action="store_true")
    ap.add_argument("--lg2", type=int, default=15)
    ap.add_argument("--password", default="password")
    ap.add_argument("--pad-random", action="store_true")
    ap.add_argument("out")
    ap.add_argument("volumes", nargs="+")
    a = ap.parse_args()
    pw = a.password.encode()
    salt = os.urandom(16)
    key, hashkey, check = kdf(pw, salt, a.lg2)
    vols = [parse(v) for v in a.volumes]

    # Group each file's parts across volumes and encrypt its whole stream.
    groups, cur = [], None
    for blocks in vols:
        for b in blocks:
            if b.type != 2 or vint(b.fields, 0)[0] & 1:  # directories: no data
                continue
            if not (b.flags & 0x08) or cur is None:
                cur = []
                groups.append(cur)
            cur.append(b)
            if not (b.flags & 0x10):
                cur = None
    for g in groups:
        iv = os.urandom(16)
        plain = b"".join(b.data for b in g)
        pad = (-len(plain)) % 16
        enc = aes_enc(key, iv, plain + (os.urandom(pad) if a.pad_random else bytes(pad)))
        at = 0
        for i, b in enumerate(g):
            n = len(b.data) + (pad if i == len(g) - 1 else 0)
            b.enc = enc[at:at + n]
            at += n
            flags = (0 if a.no_check else 1) | (0 if a.no_mac else 2)
            rec = venc(1) + venc(0) + venc(flags) + bytes([a.lg2]) + salt + iv
            if not a.no_check:
                rec += check + hashlib.sha256(check).digest()[:4]
            extra = venc(len(rec)) + rec
            # The hash record: an HMAC.
            e = 0
            old = b.extra
            while e < len(old):
                sz, e2 = vint(old, e)
                et, e3 = vint(old, e2)
                body = old[e3:e2 + sz]
                if et == 2 and not a.no_mac and body[:1] == b"\x00":
                    body = b"\x00" + hmac.new(hashkey, body[1:33], "sha256").digest()
                r = venc(et) + body
                extra += venc(len(r)) + r
                e = e2 + sz
            b.new_extra = extra
            off = crc_offset(b.fields)
            if off is not None and not a.no_mac:
                last = not (b.flags & 0x10)
                c = int.from_bytes(b.fields[off:off + 4], "little") if last else zlib.crc32(b.enc)
                b.fields = b.fields[:off] + crc_mac(hashkey, c).to_bytes(4, "little") + b.fields[off + 4:]

    os.makedirs(a.out, exist_ok=True)
    for path, blocks in zip(a.volumes, vols):
        out = bytearray(SIG)
        if a.hp:
            cb = Block()
            cb.type, cb.flags, cb.data = 4, 0, b""
            cb.fields = venc(0) + venc(0 if a.no_check else 1) + bytes([a.lg2]) + salt
            if not a.no_check:
                cb.fields += check + hashlib.sha256(check).digest()[:4]
            out += build(cb, b"", 0)
        for b in blocks:
            if hasattr(b, "enc"):
                head, data = build(b, b.new_extra, len(b.enc)), b.enc
            else:
                head, data = build(b, b.extra, len(b.data)), b.data
            if a.hp:
                iv = os.urandom(16)
                head = iv + aes_enc(key, iv, head + bytes((-len(head)) % 16))
            out += head + data
        open(os.path.join(a.out, os.path.basename(path)), "wb").write(out)


main()

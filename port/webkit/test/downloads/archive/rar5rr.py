#!/usr/bin/env python3
"""Adds a recovery record to an unencrypted RAR5 archive the way rar -rr
does: a service header named "RR" whose only extra field is EX_SUBDATA,
with data of its own, just before the end of archive header. (libarchive
3.8.1 failed on one: process_head_file_extra() returned ARCHIVE_FATAL,
with no message, for a header whose extra fields it only skips.) The data
is random: nothing reads it.
  rar5rr.py in.rar out.rar [data_size]
"""
import os, sys, zlib

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


def main():
    src, dst = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 70000
    data = open(src, "rb").read()
    assert data.startswith(SIG), src
    p = len(SIG)
    end = None
    while p < len(data):
        hsize, q = vint(data, p + 4)
        typ, r = vint(data, q)
        flags, r = vint(data, r)
        dsize = 0
        if flags & 1:
            _, r = vint(data, r)
        if flags & 2:
            dsize, r = vint(data, r)
        if typ == 5:
            end = p
            break
        p = q + hsize + dsize
    assert end is not None, "no end of archive header"
    sub = venc(7) + b"\x03"  # EX_SUBDATA; RAR puts the recovery percent here
    extra = venc(len(sub)) + sub
    name = b"RR"
    fields = venc(0) + venc(size) + venc(0) + venc(0) + venc(0) + venc(len(name)) + name
    body = venc(3) + venc(0x1 | 0x2 | 0x4) + venc(len(extra)) + venc(size) + fields + extra
    head = venc(len(body)) + body
    block = zlib.crc32(head).to_bytes(4, "little") + head + os.urandom(size)
    open(dst, "wb").write(data[:end] + block + data[end:])


main()

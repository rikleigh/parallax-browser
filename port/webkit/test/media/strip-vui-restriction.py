#!/usr/bin/env python3
"""Drop the VUI bitstream_restriction (max_num_reorder_frames,
max_dec_frame_buffering) from the SPS in an MP4's avcC, as some encoders
write their streams: a decoder must then assume the level's whole DPB
(up to 16 frames at small sizes) before the first picture comes out.

    strip-vui-restriction.py <in.mp4> <out.mp4>

The MP4 must have its moov after the mdat (no +faststart), so only the
moov's box sizes change. The flag's bit position comes from FFmpeg's
trace_headers bitstream filter.
"""
import re
import struct
import subprocess
import sys

CONTAINERS = {b"moov", b"trak", b"mdia", b"minf", b"stbl"}


def flag_bit(path):
    out = subprocess.run(["ffmpeg", "-hide_banner", "-i", path, "-c", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    m = re.search(r"\]\s+(\d+)\s+bitstream_restriction_flag\s+1 = 1", out)
    if not m:
        sys.exit("no bitstream_restriction_flag set in the SPS")
    return int(m.group(1))


def unescape(b):
    out, zeros = bytearray(), 0
    for x in b:
        if zeros >= 2 and x == 3:
            zeros = 0
            continue
        out.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return bytes(out)


def escape(b):
    out, zeros = bytearray(), 0
    for x in b:
        if zeros >= 2 and x <= 3:
            out.append(3)
            zeros = 0
        out.append(x)
        zeros = zeros + 1 if x == 0 else 0
    return bytes(out)


def strip_sps(sps, bit):
    rbsp = unescape(sps)
    bits = "".join(f"{x:08b}" for x in rbsp)[:bit] + "0" + "1"
    bits += "0" * (-len(bits) % 8)
    return escape(bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8)))


def rewrite_avcc(payload, bit):
    p = bytearray(payload[:5])
    count = payload[5] & 0x1F
    p.append(payload[5])
    i = 6
    for _ in range(count):
        n = struct.unpack(">H", payload[i:i + 2])[0]
        sps = strip_sps(payload[i + 2:i + 2 + n], bit)
        p += struct.pack(">H", len(sps)) + sps
        i += 2 + n
    return bytes(p + payload[i:])


def rewrite(data, bit):
    out, i = bytearray(), 0
    while i + 8 <= len(data):
        size, kind = struct.unpack(">I4s", data[i:i + 8])
        body = data[i + 8:i + size]
        if kind in CONTAINERS:
            body = rewrite(body, bit)
        elif kind == b"stsd":
            body = body[:8] + rewrite(body[8:], bit)
        elif kind in (b"avc1", b"avc3"):
            body = body[:78] + rewrite(body[78:], bit)
        elif kind == b"avcC":
            body = rewrite_avcc(body, bit)
        out += struct.pack(">I4s", len(body) + 8, kind) + body
        i += size
    return bytes(out)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    bit = flag_bit(src)
    out, i, seen_mdat = bytearray(), 0, False
    while i + 8 <= len(data):
        size, kind = struct.unpack(">I4s", data[i:i + 8])
        box = data[i:i + size]
        if kind == b"mdat":
            seen_mdat = True
        if kind == b"moov":
            if not seen_mdat:
                sys.exit("moov before mdat (+faststart): offsets would move")
            box = rewrite(box, bit)
        out += box
        i += size
    open(dst, "wb").write(out)


if __name__ == "__main__":
    main()

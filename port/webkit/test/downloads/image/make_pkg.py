#!/usr/bin/env python3
"""make_pkg.py out kind [options]: a synthetic package for image-test.sh,
with the header and entry layout ps5image.c reads (pkg-viewer's, psdevwiki's
PKG page). Nothing in it is signed or encrypted: it only has to look like a
package to the parser, not install.

kind:
  ps4        a PS4 CNT package: name table, param.sfo, icon0.png
  ps5        a PS5 FIH package with a CNT inside: name table, param.json, icon0.png
options:
  --retail          pkg_type bit 31 set (PS4); byte 5 = 0x80 and no CNT inside (PS5)
  --encrypted       the param.sfo/param.json and icon entries flagged encrypted
  --by-name         entries found only by name (IDs changed)
  --title ID --name NAME --version V --category C --content CID
  --icon FILE       icon0.png's bytes (default: a tiny PNG)
  --truncate N      the file cut to N bytes
  --hostile WHAT    count (a huge entry count), offset (an entry past the end),
                    names (a name offset past the table), sfo (a param.sfo whose
                    offsets point outside it)
  --bom             param.json starts with a UTF-8 byte order mark
"""
import argparse
import json
import struct
import zlib


def png(w=2, h=2):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + bytes([255, 0, 0, 255]) * w for _ in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def sfo(fields, hostile=False):
    keys = sorted(fields)
    key_table = b''
    data_table = b''
    index = b''
    for k in keys:
        v = fields[k]
        if isinstance(v, int):
            fmt, d = 0x0404, struct.pack('<I', v)
            used = maxlen = 4
        else:
            d = v.encode() + b'\0'
            used = len(d)
            maxlen = (used + 3) & ~3
            d = d.ljust(maxlen, b'\0')
            fmt = 0x0204
        index += struct.pack('<HHIII', len(key_table), fmt, used if not hostile else 0x7fffffff, maxlen, len(data_table))
        key_table += k.encode() + b'\0'
        data_table += d
    while len(key_table) % 4:
        key_table += b'\0'
    key_off = 20 + len(index)
    data_off = key_off + len(key_table)
    return b'\0PSF' + struct.pack('<IIII', 0x101, key_off, data_off, len(keys)) + index + key_table + data_table


def cnt(entries, content_id, pkg_type, hostile=None):
    """entries: (id, name, bytes, encrypted). Returns the CNT bytes."""
    names = b'\0'
    name_at = {}
    for _id, name, _data, _enc in entries:
        name_at[name] = len(names)
        names += name.encode() + b'\0'
    all_entries = [(0x0200, None, names, False)] + entries
    table_off = 0x2000
    data_off = table_off + 32 * len(all_entries)
    rows = b''
    body = b''
    for i, (eid, name, data, enc) in enumerate(all_entries):
        off = data_off + len(body)
        nat = name_at.get(name, 0)
        if hostile == 'offset' and eid != 0x0200 and i == 1:
            off = 0x7ffffff0
        if hostile == 'names' and name is not None:
            nat = 0x7ffffff0
        rows += struct.pack('>8I', eid, nat, 0x80000000 if enc else 0, 0, off, len(data), 0, 0)
        body += data
        while len(body) % 16:
            body += b'\0'
    count = len(all_entries) if hostile != 'count' else 0x7fffffff
    h = bytearray(0x2000)
    h[0:4] = b'\x7fCNT'
    struct.pack_into('>I', h, 4, pkg_type)
    struct.pack_into('>I', h, 0x10, count)
    struct.pack_into('>H', h, 0x14, len(all_entries))
    struct.pack_into('>I', h, 0x18, table_off)
    struct.pack_into('>I', h, 0x1c, len(rows))
    cid = content_id.encode()[:0x24]
    h[0x40:0x40 + len(cid)] = cid
    return bytes(h) + rows + body


def main():
    p = argparse.ArgumentParser()
    p.add_argument('out')
    p.add_argument('kind', choices=['ps4', 'ps5'])
    p.add_argument('--retail', action='store_true')
    p.add_argument('--encrypted', action='store_true')
    p.add_argument('--by-name', action='store_true')
    p.add_argument('--title', default=None)
    p.add_argument('--name', default='Test App')
    p.add_argument('--version', default=None)
    p.add_argument('--category', default=None)
    p.add_argument('--content', default=None)
    p.add_argument('--icon', default=None)
    p.add_argument('--truncate', type=int, default=None)
    p.add_argument('--hostile', default=None)
    p.add_argument('--bom', action='store_true')
    a = p.parse_args()
    title = a.title or ('CUSA12345' if a.kind == 'ps4' else 'PPSA12345')
    content = a.content or f'UP9000-{title}_00-TESTAPP000000000'
    icon = open(a.icon, 'rb').read() if a.icon else png()
    if a.kind == 'ps4':
        fields = {'TITLE': a.name, 'TITLE_ID': title, 'CONTENT_ID': content, 'CATEGORY': a.category or 'gd',
                  'VERSION': a.version or '01.00', 'APP_VER': '01.05', 'SYSTEM_VER': 0x05050000}
        meta = ('param.sfo', 0x1000, sfo(fields, a.hostile == 'sfo'))
    else:
        doc = {'titleId': title, 'contentId': content, 'contentVersion': a.version or '01.000.000',
               'applicationCategoryType': int(a.category or 0),
               'localizedParameters': {'defaultLanguage': 'en-US', 'en-US': {'titleName': a.name}}}
        text = json.dumps(doc, ensure_ascii=False).encode()
        meta = ('param.json', 0x2000, (b'\xef\xbb\xbf' if a.bom else b'') + text)
    ids = {'param.sfo': 0x1000, 'param.json': 0x2000, 'icon0.png': 0x1200}
    entries = []
    for name, eid, data in [meta, ('icon0.png', 0x1200, icon)]:
        if a.by_name:
            eid = 0x1100 + len(entries)  # not the known IDs: found by name only
        entries.append((eid, name, data, a.encrypted))
    _ = ids
    if a.kind == 'ps4':
        out = cnt(entries, content, 0x80000001 if a.retail else 0x00000001, a.hostile)
    else:
        inner = cnt(entries, content, 0x00000001, a.hostile)
        fih = bytearray(0x10000)
        fih[0:4] = b'\x7fFIH'
        fih[5] = 0x80 if a.retail else 0x00
        if not a.retail:
            struct.pack_into('<Q', fih, 0x58, len(fih))
            out = bytes(fih) + inner
        else:
            struct.pack_into('<Q', fih, 0x58, 0x8000)  # points at encrypted bytes, not a CNT
            out = bytes(fih) + bytes(range(256)) * 64
    if a.truncate is not None:
        out = out[:a.truncate]
    open(a.out, 'wb').write(out)


if __name__ == '__main__':
    main()

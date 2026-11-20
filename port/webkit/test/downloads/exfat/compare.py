#!/usr/bin/env python3
# compare.py: diff -r for a folder and its mounted exFAT image
# (exfat-test.sh). macOS's exFAT driver lists names decomposed (NFD) while
# the image holds them as the source spells them, so names are compared
# NFC-normalized; everything else (the set of files and folders, file
# bytes) must match exactly.
#   compare.py <source> <mounted image>
import os, sys, unicodedata

def tree(top):
    found = {}
    for folder, dirs, files in os.walk(top):
        for name in dirs + files:
            full = os.path.join(folder, name)
            rel = unicodedata.normalize("NFC", os.path.relpath(full, top))
            found[rel] = full
    return found

src, mnt = tree(sys.argv[1]), tree(sys.argv[2])
bad = []
for rel in sorted(set(src) ^ set(mnt)):
    bad.append("only in %s: %s" % ("the source" if rel in src else "the image", rel))
for rel in sorted(set(src) & set(mnt)):
    a, b = src[rel], mnt[rel]
    if os.path.isdir(a) != os.path.isdir(b):
        bad.append("%s: a folder on one side only" % rel)
        continue
    if os.path.isdir(a):
        continue
    if os.path.getsize(a) != os.path.getsize(b):
        bad.append("%s: %d bytes, %d in the image" % (rel, os.path.getsize(a), os.path.getsize(b)))
        continue
    with open(a, "rb") as x, open(b, "rb") as y:
        while True:
            p, q = x.read(8 << 20), y.read(8 << 20)
            if p != q:
                bad.append("%s: the bytes differ" % rel)
                break
            if not p:
                break
if bad:
    print("\n".join(bad[:20]))
    sys.exit(1)
print("%d files and folders the same" % len(src))

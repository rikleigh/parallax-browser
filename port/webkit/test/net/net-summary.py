#!/usr/bin/env python3
"""Summarise net-ab.sh logs: per load, the navigation's DNS/connect/TLS/TTFB
and load times, and over its resources: count, cache hits, how many paid a
DNS lookup / a new connection / a TLS handshake (and their median and sum),
median TTFB, protocols."""
import os, re, statistics, sys
from collections import Counter

def parse(path):
    nav, res = None, []
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("ONLOAD-JS "):
            line = line[len("ONLOAD-JS "):]
        if line.startswith("NAV ") or line.startswith("RES "):
            parts = line.split(" ")
            kind = parts[0]
            extra = {}
            while parts and "=" in parts[-1]:
                k, v = parts.pop().split("=", 1)
                extra[k] = v
            try:
                nums = [int(x) for x in parts[-8:]]
            except ValueError:
                continue
            rec = dict(name=" ".join(parts[1:-9]), proto=parts[-9], start=nums[0], dns=nums[1], connect=nums[2],
                       tls=nums[3], ttfb=nums[4], download=nums[5], transfer=nums[6], encoded=nums[7], **extra)
            if kind == "NAV":
                nav = rec
            else:
                res.append(rec)
    return nav, res

def med(values):
    return statistics.median(values) if values else 0

def main(directory):
    rows = []
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".log") or name[:1].isdigit():
            continue
        nav, res = parse(os.path.join(directory, name))
        if not nav:
            rows.append(f"{name[:-4]:<64} no timing (load failed?)")
            continue
        # A cache hit has no protocol (WebKit reports encodedBodySize 0 for it).
        hits = sum(1 for r in res if r["proto"] == "-")
        dns = [r["dns"] for r in res if r["dns"] > 0]
        conn = [r["connect"] for r in res if r["connect"] > 0]
        tls = [r["tls"] for r in res if r["tls"] > 0]
        ttfb = [r["ttfb"] for r in res if r["ttfb"] > 0 and r["proto"] != "-"]
        protos = Counter(r["proto"] for r in res)
        rows.append(f"{name[:-4]:<64} load {nav.get('load','?'):>5} dcl {nav.get('dcl','?'):>5} | nav dns {nav['dns']} conn {nav['connect']} "
                    f"tls {nav['tls']} ttfb {nav['ttfb']} dl {nav['download']} | {len(res)} res, {hits} cached, "
                    f"dns {len(dns)}x med {med(dns)} sum {sum(dns)}, conn {len(conn)}x med {med(conn)}, tls {len(tls)}x med {med(tls)}, "
                    f"ttfb med {med(ttfb)} | {dict(protos)}")
    print("\n".join(rows))

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else ".")

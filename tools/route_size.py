#!/usr/bin/env python3
"""How small a route gets for LoRa: a GPX file in, simplified and binary-encoded sizes out.

    python3 -I tools/route_size.py route.gpx [more.gpx ...]

Points are read with a plain scan for <trkpt>, <rtept> and <wpt> tags rather than an XML parser,
so a hostile file cannot expand entities. Each track segment and route is simplified with
Douglas-Peucker at several tolerances, then encoded as its first point plus zigzag-varint deltas
in 1e-5 degree units (about 1.1 m).
"""
import gzip
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from airtime import time_on_air  # noqa: E402

TAG = re.compile(rb"<(/?)(trkseg|rte|trkpt|rtept|wpt)\b([^>]*)>")
ATTR = re.compile(rb'\b(lat|lon)\s*=\s*["\']([-+0-9.eE]+)["\']')
PACKET = 230           # route bytes per LoRa packet; 16 more for header and MIC
TOLERANCES = (5, 10, 20, 50)


def read_gpx(data):
    """(lines, waypoints): each line is a list of (lat, lon) for a track segment or a route."""
    lines, waypoints, current = [], [], None
    for m in TAG.finditer(data):
        closing, tag, attrs = m.group(1), m.group(2), m.group(3)
        if tag in (b"trkseg", b"rte"):
            if closing:
                if current:
                    lines.append(current)
                current = None
            else:
                current = []
            continue
        if closing:
            continue
        a = dict(ATTR.findall(attrs))
        if b"lat" not in a or b"lon" not in a:
            continue
        p = (float(a[b"lat"]), float(a[b"lon"]))
        if tag == b"wpt":
            waypoints.append(p)
        elif current is not None:
            current.append(p)
    return lines, waypoints


def to_metres(points):
    lat0 = math.radians(points[0][0])
    k = 111_320.0
    return [(lon * k * math.cos(lat0), lat * k) for lat, lon in points]


def length_km(points):
    xy = to_metres(points)
    return sum(math.dist(a, b) for a, b in zip(xy, xy[1:])) / 1000


def simplify(points, tolerance_m):
    """Douglas-Peucker, iterative so long tracks cannot exhaust the stack. Returns kept indices."""
    if len(points) < 3:
        return list(range(len(points)))
    xy = to_metres(points)
    keep = {0, len(xy) - 1}
    stack = [(0, len(xy) - 1)]
    while stack:
        i, j = stack.pop()
        (ax, ay), (bx, by) = xy[i], xy[j]
        dx, dy = bx - ax, by - ay
        norm = math.hypot(dx, dy)
        worst, at = -1.0, None
        for k in range(i + 1, j):
            px, py = xy[k]
            if norm == 0:
                d = math.hypot(px - ax, py - ay)
            else:
                t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (norm * norm)))
                d = math.hypot(px - ax - t * dx, py - ay - t * dy)
            if d > worst:
                worst, at = d, k
        if at is not None and worst > tolerance_m:
            keep.add(at)
            stack += [(i, at), (at, j)]
    return sorted(keep)


def varint_len(v):
    z = (v << 1) ^ (v >> 63)  # zigzag
    n = 1
    while z >= 0x80:
        z >>= 7
        n += 1
    return n


def encoded_size(points):
    """First point as two 32-bit integers, then zigzag-varint deltas, in 1e-5 degree units."""
    q = [(round(lat * 1e5), round(lon * 1e5)) for lat, lon in points]
    size = 8 + 2  # first point + point count
    for (a1, o1), (a2, o2) in zip(q, q[1:]):
        size += varint_len(a2 - a1) + varint_len(o2 - o1)
    return size


def check():
    line = [(45.0, 7.0 + i * 1e-4) for i in range(100)]          # a straight line east
    assert simplify(line, 5) == [0, 99]
    assert simplify([(45.0, 7.0), (45.001, 7.001), (45.0, 7.002)], 5) == [0, 1, 2]
    assert varint_len(0) == 1 and varint_len(63) == 1 and varint_len(64) == 2 and varint_len(-64) == 1
    assert encoded_size([(45.0, 7.0), (45.00001, 7.00001)]) == 12


def report(path):
    data = open(path, "rb").read()
    lines, waypoints = read_gpx(data)
    points = sum(len(l) for l in lines)
    km = sum(length_km(l) for l in lines if len(l) > 1)
    print(f"\n{os.path.basename(path)}: {len(data) / 1e6:.2f} MB as GPX, "
          f"{len(gzip.compress(data)) / 1e6:.2f} MB gzipped")
    print(f"  {len(lines)} track segments / routes, {points} points, {km:.0f} km; {len(waypoints)} waypoints")
    if not lines:
        return
    print(f"  {'tolerance':>9} {'points':>8} {'bytes':>9} {'packets':>8}   on air once: SF7/250   SF9/250  SF10/125")
    for tol in TOLERANCES:
        kept = [[l[i] for i in simplify(l, tol)] for l in lines if l]
        n = sum(len(k) for k in kept)
        size = sum(encoded_size(k) for k in kept)
        packets = math.ceil(size / PACKET)
        t = [packets * time_on_air(PACKET + 16, sf, bw, preamble=8) for sf, bw in ((7, 250), (9, 250), (10, 125))]
        print(f"  {tol:7} m {n:8} {size:9} {packets:8}   {t[0]:19.1f} s {t[1]:7.1f} s {t[2]:7.1f} s")
    wsize = 2 + 6 * len(waypoints)
    print(f"  waypoints alone, 6 bytes each: {wsize} bytes, {math.ceil(wsize / PACKET)} packets")


if __name__ == "__main__":
    check()
    for p in sys.argv[1:]:
        report(p)

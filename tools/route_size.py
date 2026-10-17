#!/usr/bin/env python3
"""How small a route gets for LoRa: a GPX file in, simplified and binary-encoded sizes out.

    python3 -I tools/route_size.py route.gpx [more.gpx ...] [--tolerances 2,5,10]

A size estimator, not a route format: it counts bytes, it emits nothing decodable.

- Points come from a plain scan for <trkpt>, <rtept> and <wpt> tags (any namespace prefix),
  after XML comments are removed, so a hostile file cannot expand entities. Coordinates out of
  range are an error, not a point.
- Each track segment and route is simplified with Douglas-Peucker, in a local projection per
  interval, and an interval is also split wherever the path doubles back by more than the
  tolerance, so an out-and-back keeps its order of travel.
- The tolerance is a promise about the *decoded* line: simplification gets the tolerance minus
  the worst rounding of the 1e-5 degree grid, and the achieved maximum distance from every
  original point to the decoded line is measured and printed.
- Encoding: per line a type byte and a point count, the first point as two 32-bit integers, then
  zigzag-varint deltas in 1e-5 degree units (about 1.1 m); plus a 24-byte manifest per route.
"""
import gzip
import hashlib
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from airtime import time_on_air  # noqa: E402

COMMENT = re.compile(rb"<!--.*?-->", re.S)
TAG = re.compile(rb"<(/?)(?:[A-Za-z_][\w.-]*:)?(trkseg|rte|trkpt|rtept|wpt)\b([^>]*)>")
ATTR = re.compile(rb'\b(lat|lon)\s*=\s*["\']([-+0-9.eE]+)["\']')
PACKET = 230           # route bytes per LoRa packet; 16 more for header and MIC
MANIFEST = 24          # route ID, revision, segment count and types, hash: once per route
GRID = 1e-5            # degrees per encoded unit
M_PER_DEG = 111_320.0
ROUNDING_M = math.hypot(GRID / 2 * M_PER_DEG, GRID / 2 * M_PER_DEG)  # worst rounding of a point
DEFAULT_TOLERANCES = (2, 5, 10, 20, 50, 100, 200, 500)


def read_gpx(data):
    """(lines, waypoints): each line is a list of (lat, lon) for a track segment or a route."""
    data = COMMENT.sub(b"", data)
    lines, waypoints, current = [], [], None
    for m in TAG.finditer(data):
        closing, tag, attrs = m.group(1), m.group(2), m.group(3)
        if tag in (b"trkseg", b"rte"):
            if closing:
                if current:
                    lines.append(current)
                current = None
            elif not attrs.rstrip().endswith(b"/"):
                current = []
            continue
        if closing:
            continue
        a = dict(ATTR.findall(attrs))
        if b"lat" not in a or b"lon" not in a:
            continue
        lat, lon = float(a[b"lat"]), float(a[b"lon"])
        if not (-90 <= lat <= 90 and -180 <= lon <= 180):
            raise ValueError(f"coordinate out of range: {lat}, {lon}")
        if tag == b"wpt":
            waypoints.append((lat, lon))
        elif current is not None:
            current.append((lat, lon))
    return lines, waypoints


def _local(origin, p):
    """Metres east and north of origin, in a tangent plane at origin's latitude."""
    k = math.cos(math.radians(origin[0])) * M_PER_DEG
    return ((p[1] - origin[1]) * k, (p[0] - origin[0]) * M_PER_DEG)


def _distance_to_segment(p, a, b):
    """Metres from point p to segment a-b, in a plane local to a."""
    px, py = _local(a, p)
    bx, by = _local(a, b)
    nn = bx * bx + by * by
    t = 0.0 if nn == 0 else max(0.0, min(1.0, (px * bx + py * by) / nn))
    return math.hypot(px - t * bx, py - t * by)


def length_km(points):
    return sum(math.hypot(*_local(a, b)) for a, b in zip(points, points[1:])) / 1000


def simplify(points, tolerance_m):
    """Douglas-Peucker, iterative, with a local projection per interval. An interval is split at
    the point farthest from its chord, or, if the path runs backwards along the chord by more than
    the tolerance, at the point where it turned back. Returns the kept indices."""
    if len(points) < 3:
        return list(range(len(points)))
    keep = {0, len(points) - 1}
    stack = [(0, len(points) - 1)]
    while stack:
        i, j = stack.pop()
        a, b = points[i], points[j]
        bx, by = _local(a, b)
        norm = math.hypot(bx, by)
        worst, at = -1.0, None
        reach, reach_at = 0.0, i        # farthest progress along the chord so far
        for k in range(i + 1, j):
            px, py = _local(a, points[k])
            along = 0.0 if norm == 0 else (px * bx + py * by) / norm
            t = 0.0 if norm == 0 else max(0.0, min(1.0, along / norm))
            d = math.hypot(px - t * bx, py - t * by)
            if d > worst:
                worst, at = d, k
            if along > reach:
                reach, reach_at = along, k
            elif reach - along > tolerance_m and reach - along > worst:
                worst, at = reach - along, reach_at   # doubled back: keep the turning point
        if at is not None and worst > tolerance_m:
            keep.add(at)
            stack += [(i, at), (at, j)]
    return sorted(keep)


def quantize(p):
    return (round(p[0] / GRID) * GRID, round(p[1] / GRID) * GRID)


def compress(points, target_m):
    """Simplify so that the decoded line stays within target_m of every original point.
    Returns (kept indices, achieved maximum distance in metres)."""
    kept = simplify(points, max(target_m - ROUNDING_M, 0.0))
    worst = 0.0
    for i, j in zip(kept, kept[1:]):
        a, b = quantize(points[i]), quantize(points[j])
        for k in range(i, j + 1):
            worst = max(worst, _distance_to_segment(points[k], a, b))
    return kept, worst


def varint_len(v):
    z = (v << 1) ^ (v >> 63)  # zigzag
    n = 1
    while z >= 0x80:
        z >>= 7
        n += 1
    return n


def encoded_size(points):
    """A type byte, the point count, the first point as two 32-bit integers, then deltas."""
    q = [(round(lat / GRID), round(lon / GRID)) for lat, lon in points]
    size = 1 + varint_len(len(q)) + 8
    for (a1, o1), (a2, o2) in zip(q, q[1:]):
        size += varint_len(a2 - a1) + varint_len(o2 - o1)
    return size


def check():
    line = [(45.0, 7.0 + i * 1e-4) for i in range(100)]          # a straight line east
    assert simplify(line, 5) == [0, 99]
    assert simplify([(45.0, 7.0), (45.001, 7.001), (45.0, 7.002)], 5) == [0, 1, 2]
    # an out-and-back along one road keeps both turns
    assert simplify([(45, 7), (45, 7.002), (45, 7.001), (45, 7.003)], 2) == [0, 1, 2, 3]
    # rounding is budgeted: the decoded line stays within the target (the case that gave 2.449 m)
    kept, err = compress([(45.0000049, 7), (45.000022, 7.0001), (45.0000049, 7.0002)], 2)
    assert err <= 2, err
    # comments are not points; namespace prefixes are; out-of-range coordinates are errors
    assert read_gpx(b'<trk><trkseg><!--<trkpt lat="80" lon="170"/>--></trkseg></trk>') == ([], [])
    assert read_gpx(b'<g:trk><g:trkseg><g:trkpt lat="45" lon="7"/></g:trkseg></g:trk>') == ([[(45.0, 7.0)]], [])
    try:
        read_gpx(b'<trkseg><trkpt lat="91" lon="181"/></trkseg>')
        raise AssertionError("out-of-range coordinate accepted")
    except ValueError:
        pass
    assert varint_len(0) == 1 and varint_len(63) == 1 and varint_len(64) == 2 and varint_len(-64) == 1
    assert encoded_size([(45.0, 7.0), (45.00001, 7.00001)]) == 12  # 1 type + 1 count + 8 + 2


def report(path, tolerances):
    data = open(path, "rb").read()
    lines, waypoints = read_gpx(data)
    points = sum(len(l) for l in lines)
    km = sum(length_km(l) for l in lines if len(l) > 1)
    print(f"\n{os.path.basename(path)} (sha256 {hashlib.sha256(data).hexdigest()[:16]}): "
          f"{len(data) / 1e6:.2f} MB as GPX, {len(gzip.compress(data)) / 1e6:.2f} MB gzipped")
    print(f"  {len(lines)} track segments / routes, {points} points, {km:.0f} km; {len(waypoints)} waypoints")
    if not lines:
        return
    every = MANIFEST + sum(encoded_size(l) for l in lines)
    print(f"  every point on the {GRID:g}° grid: {every} bytes, {every / km * 200:.0f} per 200 km")
    print(f"  {'target':>7} {'achieved':>8} {'points':>7} {'bytes':>7} {'per 200 km':>10} {'packets':>7}"
          f"   on air once: SF7/250  SF9/250  SF10/125")
    for tol in tolerances:
        size, n, worst = MANIFEST, 0, 0.0
        for l in lines:
            kept, err = compress(l, tol)
            worst = max(worst, err)
            n += len(kept)
            size += encoded_size([l[i] for i in kept])
        packets = math.ceil(size / PACKET)   # the last, partial packet is charged as full
        t = [packets * time_on_air(PACKET + 16, sf, bw, preamble=8) for sf, bw in ((7, 250), (9, 250), (10, 125))]
        print(f"  {tol:5g} m {worst:6.2f} m {n:7} {size:7} {size / km * 200:10.0f} {packets:7}"
              f"   {t[0]:19.1f} s {t[1]:6.1f} s {t[2]:7.1f} s")
    wsize = 2 + 6 * len(waypoints)
    print(f"  waypoints alone, 6 bytes each: {wsize} bytes, {math.ceil(wsize / PACKET)} packets")


if __name__ == "__main__":
    check()
    args, tolerances = sys.argv[1:], DEFAULT_TOLERANCES
    if "--tolerances" in args:
        i = args.index("--tolerances")
        tolerances = tuple(float(x) for x in args[i + 1].split(","))
        del args[i:i + 2]
    for p in args:
        report(p, tolerances)

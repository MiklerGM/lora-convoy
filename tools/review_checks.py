#!/usr/bin/env python3
"""Review (Codex, 2026-10-17): independent [calc] checks and [code] counterexamples.

Run: python3 -I tools/review_checks.py
SF7–12 derivation: coded blocks plus preamble/header in quarter-symbol units.
Primary reference: Lora-net/sx126x_driver src/sx126x.c,
commit a10c5dfdf89788c6ac805e9fe98889de44175aa2, functions
sx126x_get_lora_time_on_air_numerator / sx126x_get_lora_time_on_air_in_ms:
https://github.com/Lora-net/sx126x_driver/blob/a10c5dfdf89788c6ac805e9fe98889de44175aa2/src/sx126x.c
Existing tools reviewed at fe4c836018fe91683540dc30c5e24aca1e9ce1cb.
Counterexamples assert existing limitations; passing is not a safety approval.
"""
from fractions import Fraction
from pathlib import Path
import math
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import airtime
import route_size


def independent_toa(payload, sf, bw_hz, cr=5, preamble=8):
    """Exact seconds, SF7–12: explicit header, CRC, LDRO assumed for symbols >16 ms."""
    assert 7 <= sf <= 12 and payload >= 0 and 5 <= cr <= 8 and bw_hz > 0
    symbol = Fraction(2**sf, bw_hz)
    divisor = 4 * (sf - (2 if symbol > Fraction(16, 1000) else 0))
    coded_bits = max(0, 8 * payload + 16 - 4 * sf + 28)
    blocks = (coded_bits + divisor - 1) // divisor
    quarter_symbols = 4 * (preamble + 12 + blocks * cr) + 1
    return quarter_symbols * symbol / 4


def check():
    # Independently calculated symbol counts, not outputs copied from airtime.py.
    assert independent_toa(56, 7, 250000, preamble=16) == Fraction(57984, 1000000)
    assert independent_toa(56, 9, 250000, preamble=16) == Fraction(190976, 1000000)
    expected_us = {7: 30848, 8: 56576, 9: 102912}
    for sf, micros in expected_us.items():
        position = independent_toa(24, sf, 250000)
        assert position == Fraction(micros, 1000000)
        frame = 20 * (position + Fraction(10, 1000)) / Fraction(4, 5)
        print(f"SF{sf}/250: position {float(position)*1000:.3f} ms; 20 riders {float(frame):.4f} s")
    assert independent_toa(246, 7, 250000) == Fraction(192128, 1000000)
    assert independent_toa(70, 7, 250000) == Fraction(64128, 1000000)
    assert independent_toa(37, 8, 62500, cr=8, preamble=32) == Fraction(508928, 1000000)
    assert independent_toa(122, 7, 250000, preamble=16) == Fraction(106624, 1000000)
    assert independent_toa(134, 7, 250000, preamble=16) == Fraction(114304, 1000000)
    position = independent_toa(24, 10, 125000)
    route = 22 * independent_toa(246, 10, 125000)
    assert position == Fraction(370688, 1000000)
    assert route == 22 * Fraction(2213888, 1000000)
    remaining_per_hour = 360 - 900 * position
    assert remaining_per_hour == Fraction(263808, 10000)
    print(f"SF10/125: route TX {float(route):.3f} s; remaining duty {float(remaining_per_hour):.4f} s/hour;")
    print(f"  {float(route / remaining_per_hour * 60):.2f} min to replenish (not a mandatory burst delay).")
    # Compare the existing implementation only AFTER the independent derivation.
    for payload in (24, 56, 70, 246):
        for sf, bw in ((7, 250000), (8, 250000), (9, 250000), (10, 125000), (12, 125000)):
            actual = airtime.time_on_air(payload, sf, bw / 1000, preamble=8)
            assert math.isclose(actual, float(independent_toa(payload, sf, bw)), abs_tol=1e-12)

    # A regex is not a GPX/XML parser: comments count; legal prefixed elements disappear.
    commented = (b'<gpx xmlns="http://www.topografix.com/GPX/1/1" version="1.1" creator="review">'
                 b'<trk><trkseg><!--<trkpt lat="80" lon="170"/>--></trkseg></trk></gpx>')
    assert route_size.read_gpx(commented)[0] == [[(80.0, 170.0)]]
    prefixed = (b'<g:gpx xmlns:g="http://www.topografix.com/GPX/1/1" version="1.1" creator="review">'
                b'<g:trk><g:trkseg>'
                b'<g:trkpt lat="45" lon="7"/></g:trkseg></g:trk></g:gpx>')
    assert route_size.read_gpx(prefixed) == ([], [])
    assert route_size.read_gpx(b'<trkseg><trkpt lat="91" lon="181"/></trkseg>')[0] == [[(91.0, 181.0)]]

    points = [(45.0000049, 7), (45.000022, 7.0001), (45.0000049, 7.0002)]
    assert route_size.simplify(points, 2) == [0, 2]
    decoded_lat = round(points[0][0] * 1e5) / 1e5
    error = (points[1][0] - decoded_lat) * 111320
    assert error > 2
    out_and_back = [(45, 7), (45, 7.002), (45, 7.001), (45, 7.003)]
    assert route_size.simplify(out_and_back, 2) == [0, 3]
    print(f"Reproduced GPX comment/namespace errors, {error:.3f} m error, and lost backtracking.")
    print("Independent airtime checks and review counterexamples passed.")


if __name__ == "__main__":
    check()

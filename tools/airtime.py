#!/usr/bin/env python3
"""LoRa time on air, duty cycle and group capacity: every number in docs/ comes from here.

    python3 tools/airtime.py             the tables the docs quote
    python3 tools/airtime.py 56 7 250    one packet: bytes, SF, bandwidth in kHz [, CR 5..8] [, preamble]

Standard library only. The formula is the SX126x datasheet's (Semtech), valid for SF7-SF12.
"""
import math
import sys


def symbol_time(sf, bw_khz):
    return 2 ** sf / (bw_khz * 1e3)


def time_on_air(payload, sf, bw_khz, cr=5, preamble=16, crc=True, implicit=False, ldro=None):
    """Seconds on air for one packet. cr is the coding rate's denominator: 5 for 4/5 ... 8 for 4/8.
    Low data rate optimisation is assumed on for symbols over 16 ms unless given; it is a
    configured PHY choice, and Semtech recommends it from 16.38 ms."""
    ts = symbol_time(sf, bw_khz)
    de = (ts > 0.016) if ldro is None else ldro
    bits = 8 * payload - 4 * sf + 28 + 16 * crc - 20 * implicit
    symbols = 8 + max(math.ceil(bits / (4 * (sf - 2 * de))) * cr, 0)
    return (preamble + 4.25) * ts + symbols * ts


def bitrate(sf, bw_khz, cr=5):
    return sf * bw_khz * 1e3 / 2 ** sf * 4 / cr


# Link budget at 22 dBm (transmit power minus SX1262 sensitivity in dBm), as Meshtastic publishes
# them; antenna gains and installation losses not included.
MODES = [("SF7/250", 7, 250, 143), ("SF8/250", 8, 250, 145.5), ("SF9/250", 9, 250, 148),
         ("SF10/250", 10, 250, 150.5), ("SF9/125", 9, 125, 151), ("SF10/125", 10, 125, 153.5)]


def plan(riders, target_s, duty=0.10, guard=0.010, packet=24):
    """The longest-range mode whose slot frame gives every rider a position every target_s or
    faster without passing the duty cycle: what a group set up for this many riders would use.
    Returns (mode, budget dB, spare slots, seconds between a rider's positions) or None.
    Budgets each rider's own positions only: relays, routes and control share the same duty
    allowance and must be added. Unlike the capacity table, it reserves no further 20 % of the
    frame, and the duty-cycle stretch of a small group is idle time the frame must include."""
    spare = max(1, round(riders / 10))
    best = None
    for name, sf, bw, budget in MODES:
        t = time_on_air(packet, sf, bw, preamble=8)
        every = max((riders + spare) * (t + guard), t / duty)  # duty cycle can stretch the frame
        if every <= target_s and (best is None or budget > best[1]):
            best = (name, budget, spare, every)
    return best


def check():
    """Against published and measured figures: if these move, every table in the docs is wrong."""
    assert round(bitrate(7, 250)) == 10938      # Meshtastic: Short Fast 10.94 kbps
    assert round(bitrate(11, 250)) == 1074      # Meshtastic: Long Fast 1.07 kbps
    assert round(time_on_air(56, 7, 250) * 1000) == 58
    assert round(time_on_air(56, 9, 250) * 1000) == 191


def tables():
    pos = 56  # a Meshtastic position on air: 16-byte header + encrypted position (approximate)
    print(f"Meshtastic EU presets, one {pos}-byte position, 16-symbol preamble")
    print(f"  {'region':9} {'preset':11} {'on air':>7} {'1 rider @30s':>13} "
          f"{'10 riders @10s, x2 relays':>26} {'@30s':>6}")
    for region, name, sf, bw, cr in [
            ("EU_868", "ShortFast", 7, 250, 5), ("EU_868", "ShortSlow", 8, 250, 5),
            ("EU_868", "MediumFast", 9, 250, 5), ("EU_868", "MediumSlow", 10, 250, 5),
            ("EU_868", "LongFast", 11, 250, 5), ("EU_866", "LiteFast", 9, 125, 5),
            ("EU_866", "LiteSlow", 10, 125, 5), ("EU_N_868", "NarrowFast", 7, 62.5, 6),
            ("EU_N_868", "NarrowSlow", 8, 62.5, 6)]:
        t = time_on_air(pos, sf, bw, cr)
        print(f"  {region:9} {name:11} {t * 1000:5.0f}ms {t / 30:12.1%} {t * 20 / 10:26.0%} {t * 20 / 30:6.0%}")

    # MeshCore sets a 32-symbol preamble at SF8 and below (RadioLibWrappers.h, preambleLengthForSF).
    print("\nMeshCore EU/UK Narrow (869.618 MHz, SF8/62.5, CR 4/8, 32-symbol preamble), 37-byte datagram:"
          f" {time_on_air(37, 8, 62.5, 8, 32) * 1000:.0f} ms")

    print("\nDMD stock (EBYTE E220, SF11/500, CR 4/5, LDRO forced on), base64 packet + 8-byte EBYTE header")
    ts = symbol_time(11, 500)
    fixed = 0.314 - time_on_air(46, 11, 500, ldro=True, preamble=-4.25)  # calibrated: 46 B -> 314 ms
    print(f"  calibrated fixed part {fixed / ts:.1f} symbols (our measurement: 46 B -> 314 ms)")
    print(f"  {'riders':>6} {'on air':>7} {'airtime':>8} {'interval':>9} {'channel':>8} {'one device':>11}")
    for n in (1, 2, 3, 5, 9):
        raw = 14 + 14 * min(n, 9)                 # header + up to nine retold riders
        onair = math.ceil(raw * 4 / 3) + 8         # base64, no padding, plus EBYTE's header
        t = fixed + time_on_air(onair, 11, 500, ldro=True, preamble=-4.25)
        interval = 1.5 + 0.5 * (n - 1)             # DMD-LoRa 1.0.9's pacing
        print(f"  {n:6} {onair:6}B {t * 1000:6.0f}ms {interval:8.1f}s {n * t / interval:8.0%} {t / interval:11.0%}")

    print("\nGroup capacity: positions per second the whole group can send")
    for name, sf in (("ShortFast", 7), ("MediumFast", 9)):
        t = time_on_air(pos, sf, 250)
        for copies, how in ((2, "one relay "), (1, "no relays ")):
            cap = 0.25 / (t * copies)              # Meshtastic withholds positions above 25% load
            print(f"  Meshtastic {name:10} {how}: {cap:4.1f}/s = 20 riders every {20 / cap:4.1f} s")
    for name, sf in (("SF7/250", 7), ("SF8/250", 8), ("SF9/250", 9)):
        t = time_on_air(24, sf, 250, preamble=8)
        slot = t + 0.010                           # 10 ms guard
        cap = 0.8 / slot                           # a schedule can use most of the frame
        print(f"  slots by GPS time {name}: {t * 1000:3.0f} ms + guard = {slot * 1000:3.0f} ms slot, "
              f"{cap:4.1f}/s = 20 riders every {20 / cap:3.1f} s, one rider @1s {t:.1%} duty")

    print("\nGroup sized at creation: the longest-range mode that still meets the cadence")
    for target in (2, 5, 10):
        for n in (5, 10, 15, 20):
            p = plan(n, target)
            print(f"  every {target:2} s, {n:2} riders (+{p[2]} spare): {p[0]:9} {p[1]:5.1f} dB, "
                  f"each rider every {p[3]:4.2f} s" if p else f"  every {target} s, {n} riders: none")

    print("\nA route over LoRa: 230 bytes of route per packet, 16 more for header and MIC")
    for what, points, per_point in (("20 via points, routed by each phone", 20, 6),
                                    ("highway 200 km, ~2 points/km", 400, 2.5),
                                    ("mountains 200 km, ~10 points/km", 2000, 2.5)):
        size = 8 + points * per_point
        packets = math.ceil(size / 230)
        times = [packets * time_on_air(min(size, 230) + 16, sf, bw, preamble=8)
                 for _, sf, bw, _ in (MODES[0], MODES[2], MODES[5])]
        # Upper bounds on airtime: a partial last packet is charged as full; manifests,
        # feedback and retries are not included. Not delivery times.
        print(f"  {what:36} {size / 1000:4.2f} kB, {packets:2} packets, on air once: "
              f"SF7/250 {times[0]:4.1f} s, SF9/250 {times[1]:4.1f} s, SF10/125 {times[2]:4.1f} s")


if __name__ == "__main__":
    check()
    if len(sys.argv) > 1:
        args = [float(a) for a in sys.argv[1:]]
        payload, sf, bw = int(args[0]), int(args[1]), args[2]
        cr = int(args[3]) if len(args) > 3 else 5
        pre = args[4] if len(args) > 4 else 16
        t = time_on_air(payload, sf, bw, cr, pre)
        print(f"{payload} B at SF{sf}/{bw:g} kHz CR 4/{cr}, preamble {pre:g}: {t * 1000:.1f} ms on air, "
              f"symbol {symbol_time(sf, bw) * 1000:.3f} ms, {bitrate(sf, bw, cr):.0f} bit/s")
    else:
        tables()

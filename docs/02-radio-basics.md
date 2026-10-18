# 02. Radio basics: SF, bandwidth, time on air, duty cycle, capacity

Everything else in these documents rests on three numbers: how long one packet occupies the air,
how much of the hour one device may transmit, and how much of the channel the whole group fills.

## LoRa parameters

A LoRa symbol is a chirp that sweeps the whole bandwidth; where the sweep starts encodes the data.

- **BW, bandwidth**: the width of the sweep, 62.5, 125, 250 or 500 kHz.
- **SF, spreading factor**: how many bits one symbol carries, 5 to 12. A symbol lasts
  `Tsym = 2^SF / BW`.
- **CR, coding rate**: forward error correction, 4/5 to 4/8: every 4 data bits go out as 5 to 8.

"SF7/250" in these documents means SF7 at 250 kHz, with CR 4/5 unless stated.

| Mode | Symbol | Bit rate | Where it appears |
|---|---|---|---|
| SF7/250 | 0.512 ms | 10.9 kbit/s | Meshtastic ShortFast |
| SF9/250 | 2.048 ms | 3.5 kbit/s | Meshtastic MediumFast |
| SF11/250 | 8.192 ms | 1.07 kbit/s | Meshtastic LongFast |
| SF11/500 | 4.096 ms | 2.1 kbit/s | DMD (EBYTE E220, "2.4k") |
| SF8/62.5, CR 4/8 | 4.096 ms | 1.0 kbit/s | MeshCore EU/UK Narrow |

Bit rate is `SF × BW / 2^SF × 4/CR`; for SF7/250 that gives the 10.94 kbit/s Meshtastic publishes.

What each step costs and buys:

- **+1 SF**: a symbol twice as long carries one bit more, so the rate almost halves, and the
  receiver gains about 2.5 dB of sensitivity.
- **Half the bandwidth**: twice the symbol time, 3 dB more sensitivity.
- **Link budget** is the path loss a link can take: transmit power minus the receiver's
  sensitivity in dBm, plus antenna gains, minus installation losses. At 22 dBm on an SX1262 and
  0 dBi antennas: ShortFast `22 − (−121) = 143 dB`, MediumFast 148 dB, LongFast 153 dB [docs].
  Six more dB double the range in free space, but only multiply it by about 1.4 near the ground.
  Sub-bands with different permitted radiated power need those powers, not these budgets (range
  by sub-band is below).

## Time on air

From the SX126x datasheet (SF7–SF12; Semtech SX1261/2 rev. 1.2, §6.1.4):

```
Tpreamble = (Npreamble + 4.25) × Tsym
Npayload  = 8 + max(ceil((8·PL − 4·SF + 28 + 16·CRC − 20·IH) / (4·(SF − 2·DE))) × CRden, 0)
Ttotal    = Tpreamble + Npayload × Tsym
```

| Symbol | Meaning |
|---|---|
| PL | payload bytes |
| CRC | 1 if the payload CRC is on |
| IH | 1 for an implicit header (length fixed and known), 0 for explicit |
| DE | 1 if low data rate optimisation (LDRO) is on. It is a configured choice: Semtech recommends it from 16.38 ms symbols; our tools assume it above 16 ms |
| CRden | coding rate denominator, 5 to 8 |
| 4.25 | the sync word and start-of-frame |

Every packet size or airtime quoted in these documents states its preamble, header mode, CRC and
LDRO, because all four change the result.

**Worked example**: a position of 56 bytes, ShortFast (SF7/250, CR 4/5, 16-symbol preamble,
explicit header, CRC on, DE off). Fifty-six bytes is our assumed size of a Meshtastic position, not
a fixed wire length.

1. `Tsym = 128 / 250 000 = 0.512 ms`.
2. Preamble: `(16 + 4.25) × 0.512 = 10.4 ms`.
3. `8·56 − 4·7 + 28 + 16 = 464`; `464 / (4·7) = 16.6`, rounded up to 17.
4. `Npayload = 8 + 17 × 5 = 93` symbols, `93 × 0.512 = 47.6 ms`.
5. Total **57.984 ms**. The same packet at MediumFast (SF9) takes **190.976 ms** [calc].
   `tools/cross_check.py` derives both independently, from Semtech's own driver.

Rules of thumb:

- +1 SF almost doubles the time; half the bandwidth doubles it;
- CR 4/5 → 4/8 multiplies the payload part by 1.6;
- bytes add roughly linearly;
- short packets at low SF are dominated by the fixed preamble and header, so halving a packet
  does not halve its time.

## Duty cycle

On terms: the regulations' **duty cycle** is the share of time a transmitter is on air, measured
over **one hour** (ETSI EN 300 220), and it applies to each device separately.

```
duty cycle = time on air per packet × packets per hour / 3600 s
```

The two EU 868 sub-bands that matter here, in their duty-cycle form:

| Sub-band | Power | Duty cycle | Per hour |
|---|---|---|---|
| 868.0–868.6 MHz | 25 mW ERP | 1 % | 36 s |
| 869.40–869.65 MHz | 500 mW ERP | 10 % | 360 s |

Three things go with these limits [docs]:

- **Duty cycle is not the only access option.** Commission Implementing Decision (EU) 2025/105
  (Annex, rows 48 and 54) also allows qualifying spectrum-access and interference-mitigation
  techniques instead. Listening before sending (CAD) is not by itself such a technique.
- **One budget per device and band.** Positions, relays, route transfers, retries and control
  messages all count against it. Moving to another frequency in the same band does not reset it
  (ETSI EN 300 220-1 V3.1.1, §§5.4.1–2).
- **ERP includes the antenna's gain and the feeder's losses** (ETSI §5.2): a transmit-power
  setting alone proves nothing about compliance.

Examples [calc]:

- a position at ShortFast (58 ms) every 10 s: 360 × 0.058 = 21 s an hour, **0.6 %**;
- DMD's stock companion, 314 ms every 1.5 s: 2400 × 0.314 = 754 s an hour, **21 %**.

Meshtastic counts its own transmit time over a rolling hour of 60 one-minute buckets, and channel
load over a separate rolling minute of six 10 s buckets. Three gates follow from them, the same in
release 2.7.26 and in `develop` [code: `airtime.h`, `airtime.cpp:304-335`,
`PositionModule.cpp:578`, `Router.cpp:485-510` at `fad52bb`; the same checks at `54e0d8d`]:

- **positions** are withheld above 25 % channel load, 40 % for the Tracker roles;
- **nodeinfo and telemetry** stop at half the duty allowance (`polite_duty_cycle_percent = 50`),
  5 % on `EU_868`. Positions are not subject to this gate;
- **everything** stops at the full allowance. `Router::send()` compares the share already used and
  reserves nothing for packets still queued (up to 16), so several can overshoot it; the phone then
  sees "Duty cycle limit exceeded". `override_duty_cycle` switches this check off.

So a companion on the node's Bluetooth API sees a rider's telemetry go quiet well before the
positions do. All of this is an implementation to test, not a proof of compliance.

Meshtastic's European regions:

| Region | In 2.7.26? | Band | Duty cycle | Presets | Note |
|---|---|---|---|---|---|
| `EU_868` | yes | 869.40–869.65 MHz | 10 % | 250 kHz presets, no Turbo | **one** 250 kHz slot, 869.525 MHz, shared with the public LongFast mesh |
| `EU_866` | no, `develop` only | 865.6–867.6 MHz | 2.5 % (10 % for routers) | Lite (125 kHz) | the periodic position interval is multiplied by 10 (`positionThrottle`) |
| `EU_N_868` | no, `develop` only | 869.40–869.65 MHz | 10 % | Narrow (62.5 kHz) | |

[code: release tag `54e0d8d` and `develop` at `fad52bb`; docs: meshtastic.org]

**`EU_866` is not an open 2 MHz band** [docs]: Decision 2025/105 (Annex row 47b, note [g])
specifies four 200 kHz windows, for data networks, with adaptive power control or an equivalent.
Its 10 % allowance is for **fixed** terrestrial network access points. Meshtastic gives 10 % to
the router roles (`getEffectiveDutyCycle()`), but setting a moving bike's radio to a router role
does not make it a fixed access point.

Meshtastic's frequencies are not the problem: its four `EU_866` channels, 125 kHz wide at 865.7,
866.3, 866.9 and 867.5 MHz, each sit inside one of the windows with 37.5 kHz to spare [code:
`RadioInterface.cpp:92,152,1349-1350,1391` at `fad52bb`; calc]. What rules the region out for a
convoy is the rest: the firmware has no adaptive power control at all [code], the 2.5 % cap
applies to mobile devices, and the windows are for data networks.

## Channel load and collisions

Duty cycle is per device. **Channel load** is the sum over every transmitter on the frequency, and
it decides how many packets collide.

With random access (each sends when it likes), the chance that nobody overlaps a packet is
`e^(−2G)` for a load `G`: about 60 % at 25 % load, 37 % at 50 %. The formula assumes packet starts
are independent (Poisson) and all packets the same length, so the vulnerable window is twice a
packet. Real LoRa does somewhat better (it listens before sending, and a stronger signal often wins
over a weaker one), but the shape holds. It is a per-packet success probability: offered airtime
and delivered updates are different capacities.

Meshtastic withholds positions above 25 % load, 40 % for the Tracker roles [code]. Neither is a
universal collision limit.

A schedule (TDMA: everyone in their own time slot) avoids collisions inside the group altogether.
Filling 70–80 % of the channel that way is a planning assumption: it needs bounded clock error,
unique slots and known packet lengths, and other users of the band still interfere.

## Group capacity

```
positions per second for the group = allowed load / (time on air × copies of each packet)
```

- **Meshtastic ShortFast**, every packet relayed once (×2), 25 % load: 0.25 / (0.058 × 2) =
  **2.2 positions/s**. That is 10 riders every ~4.6 s and 20 every ~9.3 s; with no relays, half
  that [calc]. Stock firmware sends positions on a 5 s tick at most [code: `PositionModule.cpp:503`
  at `fad52bb`], so 10 riders get 5 s, not 4.6.
- **Slots by GPS time**, a 24-byte packet (CR 4/5, explicit header, CRC, 8-symbol preamble, LDRO
  off) [calc]:

  | | SF7/250 | SF8/250 | SF9/250 |
  |---|---|---|---|
  | time on air | 30.848 ms | 56.576 ms | 102.912 ms |
  | with a 10 ms guard, 80 % of the frame | 19.6 positions/s | 12.0/s | 7.1/s |
  | 10 riders, each every | 0.511 s | 0.832 s | 1.411 s |
  | 20 riders, each every | 1.021 s | 1.664 s | 2.823 s |

  One rider sending every second at SF9 is on air 10.3 % of the time, already over a 10 % budget
  before any other traffic.

These are **offered** budgets. How fresh a shown position actually is also depends on losses,
relays, control traffic and route transfers.

## Range, roughly

The asymptotic two-ray ground model, `L = 40 log10(d) − 20 log10(ht) − 20 log10(hr)` (metres),
with antennas at 1.2 m. The upper figure has no margin; the lower one keeps about 14 dB for small
antennas, the rider's body and fading [calc]. Beyond about 9 km the earth's curvature hides two
antennas at 1.2 m (the radio horizon), so no figure goes past it. The model knows nothing about
forest, buildings or hills, so the last column is a guess, not a calculation.

The budgets here are Meshtastic's published ones: 22 dBm into 0 dBi antennas, which is 19.85 dBm
ERP and fits 869.40–869.65 MHz.

| Preset | Budget | Flat open road, line of sight [calc] | Forest, town, hills (guess) |
|---|---|---|---|
| ShortFast | 143 dB | ~2.0–4.5 km | ~1 km |
| MediumFast | 148 dB | ~2.7–6 km | ~1.5 km |
| LongFast | 153 dB | ~3.5–8 km | ~2 km |

**Range depends on the sub-band, not only on the mode.** With dipoles (2.15 dBi) at both ends,
sensitivity by formula (NF 6 dB), same model and margins [calc]:

| Mode | 868.0–868.6 MHz, 14 dBm ERP | 869.40–869.65 MHz, 22 dBm (plain SX1262) | 869.40–869.65 MHz, 27 dBm ERP |
|---|---|---|---|
| SF7/250 | 139.8 dB: 1.7–3.8 km | 147.8 dB: 2.7–6.0 km | 152.8 dB: 3.5–7.9 km |
| SF9/250 | 144.8 dB: 2.2–5.0 km | 152.8 dB: 3.5–7.9 km | 157.8 dB: 4.7–9 km (horizon) |
| SF10/125 | 150.3 dB: 3.1–6.9 km | 158.3 dB: 4.9–9 km (horizon) | 163.3 dB: 6.5–9 km (horizon) |

- **868.0–868.6 MHz** costs 8 dB against 22 dBm into a dipole, about 0.63 of the distance, on top
  of its 1 % duty cycle.
- **27 dBm ERP** takes an amplifier or about 7 dBi of antenna. Of our boards only the Heltec V4 has
  an amplifier, to about 28 dBm [code: `LoRaFEMInterface.cpp:290-291` at `fad52bb`]; a plain
  SX1262 stops at 22 dBm. A 7 dBi antenna squeezes its beam to a flat disc, which misses riders
  above or below on a hillside and tilts with the bike in a lean.
- **Receive antenna gain counts in full.** At the transmitter it counts toward ERP.

The mode should be chosen on measured delivery and position age ([07](07-experiments.md), E2), and
a rider whose path fails is shown with an explicitly stale last-known position.

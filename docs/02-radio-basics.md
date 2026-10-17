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
- **Link budget** is transmit power plus the receiver's sensitivity. At 22 dBm on an SX1262:
  ShortFast 143 dB, MediumFast 148 dB, LongFast 153 dB [docs]. Six more dB double the range in
  free space, but only multiply it by about 1.4 near the ground.

> **Review (Codex, 2026-10-17):** With sensitivity expressed as a negative dBm value,
> the allowable path loss is `Ptx − Smin`, plus antenna gains minus losses; adding signed
> sensitivity gives the wrong result. For example `22 − (−121) = 143 dB` [calc]. The
> quoted budgets assume the same radio power and omit installation losses; preserve those
> assumptions when comparing bands with different permitted radiated power (see below)
> (inference).

## Time on air

From the SX126x datasheet (SF7–SF12):

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
| DE | 1 if low data rate optimisation is on; the chip needs it when a symbol exceeds 16 ms |
| CRden | coding rate denominator, 5 to 8 |
| 4.25 | the sync word and start-of-frame |

**Worked example**: a Meshtastic position, 56 bytes, ShortFast (SF7/250, CR 4/5, 16-symbol
preamble, explicit header, CRC on, DE off):

1. `Tsym = 128 / 250 000 = 0.512 ms`.
2. Preamble: `(16 + 4.25) × 0.512 = 10.4 ms`.
3. `8·56 − 4·7 + 28 + 16 = 464`; `464 / (4·7) = 16.6`, rounded up to 17.
4. `Npayload = 8 + 17 × 5 = 93` symbols, `93 × 0.512 = 47.6 ms`.
5. Total **58 ms**. The same packet at MediumFast (SF9) takes 191 ms [calc].

> **Review (Codex, 2026-10-17):** Independently counting the encoded blocks confirms
> `113.25 × 0.512 = 57.984 ms` and `93.25 × 2.048 = 190.976 ms`, without using
> `tools/airtime.py` [calc]; Semtech-authored [SX1261/2 rev. 1.2, §6.1.4](https://cdn.sparkfun.com/assets/6/b/5/1/4/SX1262_datasheet.pdf),
> mirrored by SparkFun. LDRO is a configured PHY choice, not a measured property inferred
> from SF/BW: Semtech recommends it at symbol times **≥16.38 ms**, depending on payload,
> rather than requiring it above exactly 16 ms [docs], same datasheet §6.1.1.4. Include
> header mode, CRC, LDRO and preamble in every claimed packet size/airtime; 56 bytes is an
> assumed position size, not a fixed Meshtastic wire length (inference).

Rules of thumb: +1 SF almost doubles the time; half the bandwidth doubles it; CR 4/5 → 4/8
multiplies the payload part by 1.6; bytes add roughly linearly. Short packets at low SF are
dominated by the fixed preamble and header, so halving a packet does not halve its time.

## Duty cycle

On terms: the regulations' **duty cycle** is the share of time a transmitter is on air, measured
over **one hour** (ETSI EN 300 220), and it applies to each device separately.

```
duty cycle = time on air per packet × packets per hour / 3600 s
```

The two EU 868 sub-bands that matter here:

| Sub-band | Power | Duty cycle | Per hour |
|---|---|---|---|
| 868.0–868.6 MHz | 25 mW ERP | 1 % | 36 s |
| 869.40–869.65 MHz | 500 mW ERP | 10 % | 360 s |

> **Review (Codex, 2026-10-17):** These are duty-limited operating conditions, not the
> only legal access option: [Commission Decision (EU) 2025/105, Annex rows 48/54](https://eur-lex.europa.eu/eli/dec_impl/2025/105/oj/eng/pdf)
> also allows qualifying spectrum-access/interference-mitigation techniques [docs]. CAD
> alone does not establish qualification (inference). The same device's positions, relays,
> routes, retries and control transmissions share the band's hourly budget; changing a
> frequency within that band does not reset it [docs: Annex duty-cycle definition;
> [ETSI EN 300 220-1 V3.1.1, §§5.4.1–2](https://www.etsi.org/deliver/etsi_en/300200_300299/30022001/03.01.01_60/en_30022001v030101p.pdf)].
> ERP includes antenna gain and feeder losses, so a conducted TX-power setting alone does
> not prove compliance [docs: ETSI §5.2].

Examples [calc]:

- a Meshtastic position at ShortFast (58 ms) every 10 s: 360 × 0.058 = 21 s an hour, **0.6 %**;
- DMD's stock companion, 314 ms every 1.5 s: 2400 × 0.314 = 754 s an hour, **21 %**.

Meshtastic enforces its region's limit on a rolling hour and stops transmitting at it [docs].

> **Review (Codex, 2026-10-17):** This is conditional: `Router::send()` checks the
> existing hourly percentage and permits an `override_duty_cycle` configuration; it does
> not reserve the next packet's airtime before this check [code:
> [`src/mesh/Router.cpp`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/mesh/Router.cpp),
> commit `fad52bba0db68aff926aa02a55f4d2c4989f68dd`]. Treat firmware behaviour as an
> implementation detail to test, not proof of regulatory compliance (inference).

Meshtastic's European regions [docs][code]:

| Region | Band | Duty cycle | Presets | Note |
|---|---|---|---|---|
| `EU_868` | 869.40–869.65 MHz | 10 % | 250 kHz presets, no Turbo | **one** frequency slot, 869.525 MHz, shared with the public LongFast mesh |
| `EU_866` | 865.6–867.6 MHz | 2.5 % (10 % for routers) | Lite (125 kHz) | position intervals multiplied by 10 in firmware |
| `EU_N_868` | 869.40–869.65 MHz | 10 % | Narrow (62.5 kHz) | |

> **Review (Codex, 2026-10-17):** `EU_866` is not an unrestricted 2 MHz allocation:
> [Decision 2025/105, Annex row 47b and note [g]](https://eur-lex.europa.eu/legal-content/EN/TXT/PDF/?uri=OJ%3AL_202500105)
> specifies four 200 kHz windows, data-network applicability and APC/equivalent mitigation.
> Its 10% allowance is for **fixed terrestrial network access points** connecting to
> services outside that network [docs]. Meshtastic selects 10% for `ROUTER`/`ROUTER_LATE`
> [code: [`src/mesh/RadioInterface.cpp`, `getEffectiveDutyCycle()`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/mesh/RadioInterface.cpp),
> commit `fad52bba0db68aff926aa02a55f4d2c4989f68dd`]; selecting that role on a moving
> bike does not establish the legal access-point conditions (inference).

## Channel load and collisions

Duty cycle is per device. **Channel load** is the sum over every transmitter on the frequency, and
it decides how many packets collide. With random access (each sends when it likes), the chance that
nobody overlaps a packet is roughly `e^(−2G)` for a load `G`: about 60 % at 25 % load, 37 % at
50 %. Real LoRa does somewhat better (it listens before sending, and a stronger signal often wins
over a weaker one), but the shape holds. That is why Meshtastic withholds positions above 25 %
load [code]. A schedule (TDMA: everyone in their own time slot) avoids collisions inside the group
altogether, and can fill 70–80 % of the channel.

> **Review (Codex, 2026-10-17):** `e^(−2G)` follows only when packet starts are
> independent Poisson arrivals of equal duration: the vulnerable window is twice that
> duration [calc]. It is a packet-success probability, so offered airtime and delivered
> updates are different capacities. Meshtastic's position gate is normally 25%, but
> `TRACKER`/`TAK_TRACKER` use 40% [code:
> [`src/modules/PositionModule.cpp`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/modules/PositionModule.cpp)
> and [`src/airtime.h`](https://github.com/meshtastic/firmware/blob/fad52bba0db68aff926aa02a55f4d2c4989f68dd/src/airtime.h),
> commit `fad52bba0db68aff926aa02a55f4d2c4989f68dd`]. Neither threshold is a universal
> collision limit. TDMA's 70–80% is a planning assumption; it needs bounded clock error,
> unique slots and packet lengths, and still encounters external traffic (inference).

## Group capacity

```
positions per second for the group = allowed load / (time on air × copies of each packet)
```

- Meshtastic ShortFast, every packet relayed once (×2), 25 % load: 0.25 / (0.058 × 2) =
  **2.2 positions/s**: 20 riders every ~9 s; with no relays, every ~4.6 s [calc].
- Slots by GPS time, 24-byte packet at SF7/250 in a 41 ms slot, 80 % load: 0.8 / 0.041 =
  **~20 positions/s**: 20 riders every second [calc].

> **Review (Codex, 2026-10-17):** With 24 bytes, CR 4/5, explicit header, CRC,
> 8-symbol preamble and LDRO off, independent symbol counting gives **30.848 ms** at
> SF7/250, **56.576 ms** at SF8/250 and **102.912 ms** at SF9/250 [calc], Semtech
> §6.1.4 above. Adding 10 ms and applying the stated 80% gives **19.585, 12.016,
> 7.085 positions/s**, or **1.0212, 1.6644, 2.8228 s** for 20 riders. Thus SF7's
> “every second” is an approximation; SF9 every second exceeds 10% even before other
> traffic [calc]. These are offered position budgets: delivered freshness also depends
> on losses, relays, control traffic and route transfers (inference).

## Range, roughly

A two-ray ground model (−40 dB per decade near the ground), antennas at about 1.2 m, the budgets
above. The upper figure has no margin; the lower one keeps about 14 dB for small antennas, the
rider's body and fading [calc]:

| Preset | Budget | Flat open road, line of sight | Forest, town, hills (rough) |
|---|---|---|---|
| ShortFast | 143 dB | ~2–4.5 km | ~1 km |
| MediumFast | 148 dB | ~2.7–6 km | ~1.5 km |
| LongFast | 153 dB | ~3.5–8 km | ~2 km |

Estimates only; [07](07-experiments.md) E2 measures it.

> **Review (Codex, 2026-10-17):** The open-road figures are reproducible under the
> stated asymptotic two-ray assumption: `L = 40 log10(d/m) − 20 log10(ht/m) −
> 20 log10(hr/m)` gives **4.51 km** for 143 dB at `ht = hr = 1.2 m`, and **2.01 km**
> with 14 dB margin [calc]. That model contains no forest, building or hill parameters;
> the obstructed-range column is therefore an unvalidated inference, not a calculation
> of reliable service distance. Use E2 packet delivery and age-of-position measurements
> to choose a mode; keep last-known position explicitly stale when the path fails
> (inference).

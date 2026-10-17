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
  Comparing bands with different permitted radiated power needs those powers, not these budgets.

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

Meshtastic tracks its region's limit on a rolling hour [docs]. Before sending, `Router::send()`
checks the share already used; it does not reserve the next packet's airtime, and
`override_duty_cycle` switches the check off [code: firmware `develop` at `fad52bb`]. That is an
implementation to test, not a proof of compliance.

Review (Codex qwen, 2026-10-17): confirmed in `develop` at `fad52bb`, and two things to
add. The rolling hour is 60 one-minute buckets of our own transmission (`airtime.h:87,198`,
`airtime.cpp:62,215-226`), and the channel-load view is a separate rolling 60 s in six 10 s
buckets (`airtime.h:81`, `airtime.cpp:114-116`) — so the two limits are measured over different
windows, which matters when reading a "skip send" log line. [code]

There is a **second, stricter gate that this section does not mention**, and its exact scope
matters for a convoy. `AirTime::isTxAllowedAirUtil()` (`airtime.cpp:319-335`) refuses a send once
the hourly TX share passes `effectiveDutyCycle × polite_duty_cycle_percent / 100`, with
`polite_duty_cycle_percent = 50` (`airtime.h:232`). On `EU_868` that is **5 %**, not 10 %. It is
called by `NodeInfoModule.cpp:246`, `DeviceTelemetry.cpp:32`, `EnvironmentTelemetry.cpp:469`,
`HealthTelemetry.cpp:85`, `MeshBeaconModule.cpp:619` and `NeighborInfoModule.cpp:133` — but
**not** by `PositionModule.cpp`, whose only gate is the channel-load one at `:578`. So in
practice: nodeinfo and telemetry thin out at half the allowance while positions keep going, and
positions are stopped only by the hard abort in `Router.cpp:485-510` at the **full** duty cycle,
which returns `DUTY_CYCLE_LIMIT` and pushes the user-visible "Duty cycle limit exceeded. You can
send again in N mins". A companion built on the node's BLE API will therefore see a rider's
telemetry go quiet well before their positions do; that is a real failure mode for E6. [code]

The hard abort also confirms the paragraph's point that the next packet's airtime is not reserved:
the check compares the share already used, so a packet can always overshoot the limit by one
packet. [code]

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

Review (Codex qwen, 2026-10-17): this is right, and the frequency part is better than the
paragraph suggests — Meshtastic's `EU_866` channel plan lands exactly in the four legal windows,
so the frequencies are not the problem. With `RDEF(EU_866, 865.6, 867.6, …)` and
`PROFILE_LITE = {…, spacing 0.4, padding 0.0375}` (`RadioInterface.cpp:92,152`), the centre is
`865.6 + 125/2000 + 0.0375 + n × 0.6` MHz (`:1391`) and the slot count is
`round((867.6 − 865.6 + 0.4) / 0.6) = 4` (`:1349-1350`), giving centres **865.7, 866.3, 866.9,
867.5 MHz**. Each 125 kHz channel then occupies 865.6375–865.7625, 866.2375–866.3625,
866.8375–866.9625 and 867.4375–867.5625 MHz, each inside one of row 47b's windows
(865.6–865.8, 866.2–866.4, 866.8–867.0, 867.4–867.6) with at least 37.5 kHz to spare. [code, docs]

What is left is the part the paragraph names, and it is not small. Row 47b makes **Adaptive Power
Control required** (or an equivalent mitigation), and I can find no adaptive power control anywhere
in the firmware: a case-insensitive search for "adaptive power" and "APC" across `develop` at
`fad52bb` returns nothing, and the region definition fixes the power at 27 dBm
(`RadioInterface.cpp:152`). [code] So `EU_866` needs an equivalent mitigation to be argued for,
plus the 2.5 % mobile cap and note [g]'s "only available for data networks". The practical reading
stays as written: `EU_866` is the wrong region for a moving convoy, but for the reason of APC and
the mobile cap, not the frequencies. [inference]

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
  **2.2 positions/s**. That is 20 riders every ~9 s; with no relays, every ~4.6 s [calc].
- **Slots by GPS time**, a 24-byte packet (CR 4/5, explicit header, CRC, 8-symbol preamble, LDRO
  off) [calc]:

  | | SF7/250 | SF8/250 | SF9/250 |
  |---|---|---|---|
  | time on air | 30.848 ms | 56.576 ms | 102.912 ms |
  | with a 10 ms guard, 80 % of the frame | 19.6 positions/s | 12.0/s | 7.1/s |
  | 20 riders, each every | 1.021 s | 1.664 s | 2.823 s |

  One rider sending every second at SF9 is on air 10.3 % of the time, already over a 10 % budget
  before any other traffic.

These are **offered** budgets. How fresh a shown position actually is also depends on losses,
relays, control traffic and route transfers.

## Range, roughly

The asymptotic two-ray ground model, `L = 40 log10(d) − 20 log10(ht) − 20 log10(hr)` (metres),
with antennas at 1.2 m and the budgets above. The upper figure has no margin; the lower one keeps
about 14 dB for small antennas, the rider's body and fading [calc]. The model knows nothing about
forest, buildings or hills, so the last column is a guess, not a calculation:

| Preset | Budget | Flat open road, line of sight [calc] | Forest, town, hills (guess) |
|---|---|---|---|
| ShortFast | 143 dB | ~2.0–4.5 km | ~1 km |
| MediumFast | 148 dB | ~2.7–6 km | ~1.5 km |
| LongFast | 153 dB | ~3.5–8 km | ~2 km |

The mode should be chosen on measured delivery and position age ([07](07-experiments.md), E2), and
a rider whose path fails is shown with an explicitly stale last-known position.

Review (Codex qwen, 2026-10-17): the table reproduces exactly with my own calculation
(143 dB → 4.51 km, 148 → 6.01, 153.5 → 8.25; each −14 dB → 2.01, 2.69, 3.69 km), and the "six
more dB buys about 1.4×" rule of thumb above is right for a 40 dB/decade model
(10^(6/40) = 1.41). [calc]

The gap is the one this section names and then does not close: every budget here is quoted at
**22 dBm into 0 dBi antennas**, which is neither of the two sub-bands' permitted ERP. Row 54 allows
**500 mW = 27 dBm ERP** in 869.40–869.65 MHz and row 48 only **25 mW = 14 dBm ERP** in
868.0–868.6 MHz, so the same mode is worth ±5 dB depending on where it is used. Redoing the same
model at the permitted ERP, no margin / with the 14 dB margin: [calc]

| Mode | at 27 dBm ERP (869.4–869.65) | at 22 dBm (as tabulated) | at 14 dBm ERP (868.0–868.6) |
|---|---|---|---|
| SF7/250 | 148 dB: 6.0 / 2.7 km | 143 dB: 4.5 / 2.0 km | 135 dB: 2.9 / 1.3 km |
| SF9/250 | 153 dB: 8.0 / 3.6 km | 148 dB: 6.0 / 2.7 km | 140 dB: 3.8 / 1.7 km |
| SF10/125 | 158.5 dB: 11.0 / 4.9 km | 153.5 dB: 8.3 / 3.7 km | 145.5 dB: 5.2 / 2.3 km |

Two consequences for the documents. The range quoted for a mode is not a property of the mode, so
[06](06-own-protocol.md)'s "range like MediumFast" for SF9/250 should say at which ERP; and the
868.0–868.6 column is the honest answer to "could we use the other band" — it costs about 5 dB,
which is roughly a third of the distance, on top of its 1 % duty cycle. Antenna gain is worth
saying out loud too: 5 dB of antenna is 1.33× distance in this model, and ERP is where it gets
counted ([02](02-radio-basics.md) already notes ETSI §5.2). [calc]

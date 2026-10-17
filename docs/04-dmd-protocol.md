# 04. The DMD protocol: an assessment

DMD2 shares group positions over LoRa through DMD's devices (NOR7E) and LoRa modules. We recovered
the on-air format, implemented it, and rode with it in a mixed group with a NOR7E, from 2026-09-27
to 10-02. The facts here come from
[dmd2-lora-bridge](https://github.com/MiklerGM/dmd2-lora-bridge): `docs/PROTOCOL.md`,
`docs/PHY-notes.md`, `docs/ROADMAP.md` (M6), and our own measurements.

## In brief

The format is a sensible compact record of positions, but the protocol around it is not built for
a group:

- retelling everyone's position at a fast pace fills the channel by five riders;
- how fresh a position is, is not defined;
- there is no real protection, and relaying is broken.

It works for two or three riders close together.

## What it gets right

- A compact record: 14 bytes per rider, 24-bit latitude and longitude (about 2.4 × 2.8 m), a
  version byte, a per-sender sequence number that shows losses, and status bits. "Needs help"
  worked over LoRa on our rides.
- The channel follows from the group ID (channels 14–22 in Europe), so different groups land on
  different frequencies.
- It runs on inexpensive EBYTE modules in transparent mode, with a long-range PHY (SF11/500).
- On the rides, positions crossed both ways between our devices and a NOR7E.

> **Review (Codex, 2026-10-17):** [code][calc] The coordinate resolution is latitude-dependent:
> `Codec.kt` uses 46 603.375 counts/degree latitude and 23 301.6875 longitude, giving about
> 2.39 m north–south and `4.78 × cos(latitude)` m east–west (3.38 m at 45°).
> Nine EU channel choices also cannot guarantee different frequencies for different groups.
> Sources: [`Codec.kt`](https://github.com/MiklerGM/dmd2-lora-bridge/blob/2234b5a4077479bf4145c86d76d4061af515bd6f/android/app/src/main/java/com/thorkracing/dmdLora/protocol/Codec.kt)
> and [`docs/PROTOCOL.md`, §3](https://github.com/MiklerGM/dmd2-lora-bridge/blob/2234b5a4077479bf4145c86d76d4061af515bd6f/docs/PROTOCOL.md)
> at commit `2234b5a4077479bf4145c86d76d4061af515bd6f`.

## Where it breaks

1. **It does not scale** [calc]. Every packet retells everyone heard (up to nine riders), and the
   stock companion (DMD-LoRa 1.0.9) sends every 1.5 s plus 0.5 s for each LoRa rider heard. Load
   grows roughly with the square of the group. Airtime is calibrated on our measurement (46 bytes
   → 314 ms on the air):

   | Riders | Packet on air | Airtime | Interval | Channel load | One device |
   |---|---|---|---|---|---|
   | 1 | 46 B | 314 ms | 1.5 s | 21 % | 21 % |
   | 3 | 83 B | 478 ms | 2.5 s | 57 % | 19 % |
   | 5 | 120 B | 642 ms | 3.5 s | 92 % | 18 % |
   | 9 | 195 B | 990 ms | 5.5 s | 162 % | 18 % |

   > **Review (Codex, 2026-10-17):** [calc] This table does not show quadratic load: with packet
   > airtime approximately `a + bN` and the stated interval `1 + 0.5N`, offered load is
   > `N(a + bN)/(1 + 0.5N)`, approximately linear as N grows, until the nine-record cap.
   > Saturation remains a real concern; the “square” description applies to a fixed interval.
   > These are **offered** airtime sums, not measured busy time or delivery probabilities;
   > overlapping transmissions can make the sum exceed 100%. The same calibrated model
   > already exceeds 100% at **six riders** (139 B, 723.6 ms, 4 s interval: 108.54%), not first at nine.

   From nine riders the offered load exceeds the channel, so most packets collide. On 09-29, with
   three devices, each of ours received about 45 % of the other's packets; 49 % on 10-02 [bench].
   That is consistent with the table, though those days had other losses too: an E220 on 3V3
   transmitting 20–30 dB weak, and two of our devices falling into step and deafening each other.
2. **Wasted airtime.** The binary packet goes out as base64 (+33 %), behind EBYTE's 8-byte header.
   A nine-rider packet takes about one second.
3. **Freshness is undefined.** A position's age mixes clocks: the wall clock, GPS time and the
   server's. A timestamp "in the future" freezes a rider, and a retold position cannot be told
   from a fresh one. The result is frozen dots on everyone's map.
4. **Relaying is broken.** The stock relay decrements the TTL but leaves the CRC stale, and in our
   logs relaying crowded out the device's own transmissions. Our bridge stopped relaying (TTL 1).

   > **Review (Codex, 2026-10-17):** [code] Distinguish the stock defect from current bridge
   > behavior: `Codec.decode()` retries CRC with `TTL+1` and accepts that single-hop stock relay,
   > while `TX_TTL=1` avoids requesting further relays. The wire format also **does carry a
   > three-byte position age**, though no absolute fix time; the freshness problem is the
   > producers' inconsistent age semantics, not an absent age field. Source:
   > [`Codec.kt`, `decode()` / `encodeGroup()`](https://github.com/MiklerGM/dmd2-lora-bridge/blob/2234b5a4077479bf4145c86d76d4061af515bd6f/android/app/src/main/java/com/thorkracing/dmdLora/protocol/Codec.kt),
   > commit `2234b5a4077479bf4145c86d76d4061af515bd6f`.
5. **No acknowledgement or retry**, so chat is unreliable; our bridge sends each line three times.
6. **No real protection.**
   - The body is XORed with a repeating pad derived from the group ID.
   - The group selector and the sender's CRC travel in clear.
   - Nothing is authenticated: anyone who knows a group's ID can read and forge positions.

7. **The PHY is EBYTE's black box.** Low data rate optimisation is forced on, and the two sync
   symbols are a non-standard pair (40 and 36). Other chips interoperate only by imitating that;
   we found it with an RTL-SDR.
8. **At most nine riders per packet**, and the stock app takes the first nine in DMD2's order, so
   the sender's own position may be left out of its own packet.
9. **Regulation, for information.** A NOR7E asks for up to 30 dBm with listen-before-talk off on
   868.125 MHz, and one device is on air 16–21 % of the time [calc]. That part of the band allows
   25 mW and 1 %.

   > **Review (Codex, 2026-10-17):** [docs] The EU limit is **ERP**, whereas “asks for 30 dBm”
   > is a configuration request, not measured radiated power. Inference: the modeled duty cycle
   > is incompatible with the stated 1% duty-cycle access option if actually transmitted,
   > but this alone is not a measured compliance verdict on a NOR7E. Source:
   > [Commission Decision (EU) 2025/105, Annex, row 48](https://eur-lex.europa.eu/eli/dec_impl/2025/105/oj/eng/pdf).

## Lessons for anything we build

- Each device sends **its own** position; relaying is separate and selective.
- Packets stay **binary and small**.
- One time base everywhere: **GPS time**, with the age computed against the time of the fix.
- Encrypt and authenticate.
- Pace by the channel, not by a fixed interval per rider.
- Stay inside the duty cycle and power of the sub-band used.

# 04. The DMD protocol: an assessment

DMD2 shares group positions over LoRa through DMD's devices (NOR7E) and LoRa modules. We recovered
the on-air format, implemented it, and rode with it in a mixed group with a NOR7E, from 2026-09-27
to 10-02. The facts here come from
[dmd2-lora-bridge](https://github.com/MiklerGM/dmd2-lora-bridge) at commit `2234b5a`:
`docs/PROTOCOL.md`, `docs/PHY-notes.md`, `docs/ROADMAP.md` (M6), the app's `Codec.kt`, and our own
measurements.

## In brief

The format is a sensible compact record of positions, but the protocol around it is not built for
a group:

- retelling everyone's position at a fast pace fills the channel by five or six riders;
- the age a position carries is computed on inconsistent clocks;
- there is no real protection, and relaying is broken.

It works for two or three riders close together.

## What it gets right

- **A compact record**, 14 bytes per rider [code]:
  - 24-bit latitude and longitude on different scales: about 2.39 m north–south, and
    `4.78 × cos(latitude)` m east–west (3.38 m at 45°). Each field uses only 2^23 of its 2^24
    codes, so longitude could have had latitude's scale [calc];
  - a three-byte position age;
  - status bits: "needs help" worked over LoRa on our rides.

  Around the records: a version byte, and a per-sender sequence number that shows losses.
- **Groups are spread over frequencies:** the channel follows from the group ID, and the frequency
  is 850.125 MHz plus the channel. In Europe that is channels 14–22, 864.125–872.125 MHz in 1 MHz
  steps, 868.125 MHz by default [code: `docs/PROTOCOL.md:147-150`]. With nine channels, two groups
  can still land on the same one, and in the EU the plan has a cost (item 9 below).
- **Cheap hardware:** it runs on inexpensive EBYTE modules in transparent mode, with a long-range
  PHY (SF11/500).
- **It worked on the rides:** positions crossed both ways between our devices and a NOR7E.

## Where it breaks

1. **It saturates early** [calc].
   - Every packet retells everyone heard (up to nine riders).
   - The stock companion (DMD-LoRa 1.0.9) sends every 1.5 s plus 0.5 s for each LoRa rider heard.
   - Packets grow with the group and so does the interval, so the offered load grows roughly
     linearly with the number of riders, until the nine-rider cap, and it passes the channel's
     capacity at **six** riders.

   Airtime is calibrated on our measurement (46 bytes → 314 ms on air). The figures are offered
   airtime sums, not measured busy time, and they can exceed 100 % because transmissions overlap:

   | Riders | Packet on air | Airtime | Interval | Channel load | One device |
   |---|---|---|---|---|---|
   | 1 | 46 B | 314 ms | 1.5 s | 21 % | 21 % |
   | 3 | 83 B | 478 ms | 2.5 s | 57 % | 19 % |
   | 5 | 120 B | 642 ms | 3.5 s | 92 % | 18 % |
   | 6 | 139 B | 724 ms | 4.0 s | 109 % | 18 % |
   | 9 | 195 B | 990 ms | 5.5 s | 162 % | 18 % |

   On 09-29, with three devices, each of ours received about 45 % of the other's packets; on 10-02
   the tablet and the NOR7E heard each other 49 % of the time, all three riders within 10–300 m
   [bench]. That is consistent with the table. Two known losses came on top, smaller than they
   look: the phone's E220 ran on 3V3 and transmitted 20–30 dB weak, though the NOR7E still heard
   it most of the time (the owner's account); and until 10-01 two of our devices could fall into
   step and deafen each other, which randomised pacing fixed for 10-02 [bench: dmd2-lora-bridge
   `docs/ROADMAP.md`].
2. **Wasted airtime.** The binary packet goes out as base64 (+33 %), behind EBYTE's 8-byte header.
   A nine-rider packet takes about one second.
3. **Ages on inconsistent clocks.** Each record carries a three-byte position age, but no absolute
   fix time, and the senders compute that age on a mix of clocks: the wall clock, GPS time and the
   server's. A timestamp "in the future" freezes a rider, and a retold position cannot be told from
   a fresh one. The result is frozen dots on everyone's map.
4. **Relaying is broken.** The stock relay decrements the TTL but leaves the CRC stale, and in our
   logs relaying crowded out the device's own transmissions. Our bridge accepts such a single-hop
   stock relay (it retries the CRC with TTL+1) and sends with TTL 1, so it never asks for more
   [code].
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
9. **Regulation, for information.** A NOR7E's configuration asks for up to 30 dBm with
   listen-before-talk off on 868.125 MHz, and by our model one device is on air 16–21 % of the time
   [calc]. That part of the band allows 25 mW ERP and 1 % duty cycle in its duty-cycle form
   (Decision (EU) 2025/105, Annex row 48). The configured power is not a measured ERP, and the model
   is not a measurement of a NOR7E, so this is not a compliance verdict; but the modelled duty cycle
   could not fit 1 % if transmitted as modelled.

   The other channels fare no better, each 500 kHz wide [calc, against Decision 2025/105]:
   - none of the nine falls in 869.40–869.65 MHz, the one sub-band with 500 mW and 10 %;
   - 864.125–868.125 MHz lie in sub-bands of 25 mW with 0.1 % or 1 %, and 865.125 MHz straddles
     the two;
   - 869.125 MHz spans the 0.1 % sub-band and the channels kept for alarms (rows 50–53);
   - 870.125 MHz is half outside 863–870 MHz, and 871.125 and 872.125 MHz entirely. Above 870 MHz
     the EU harmonises only 874–874.4 MHz, under a separate decision (2018/1538), for RFID and
     networked devices in data networks.

## Lessons for anything we build

- Each device sends **its own** position; relaying is separate and selective.
- Packets stay **binary and small**.
- Positions carry an **absolute fix time** on one time base, GPS time, not an age computed on
  whatever clock the sender has.
- An implausible age, negative or far beyond the cadence, shows as **"stale, age unknown"**: never
  as a frozen dot, and never silently dropped.
- Identify riders by an **assigned slot index**, not by a hash of who they are. DMD's rider id is
  `CRC-16(user_id)` [code: `docs/PROTOCOL.md:91`], so two of 20 riders share one with a 0.29 %
  chance, and the chance grows with the square of the group [calc].
- A sender is **always in its own packet**: any list with a fixed capacity needs that rule.
- Encrypt and authenticate.
- Pace by the channel, not by a fixed interval per rider.
- Spread groups over frequencies only within sub-bands whose limits suit the protocol; in the EU
  that leaves little room ([06](06-own-protocol.md), ideas).
- Stay inside the duty cycle and power of the sub-band used.

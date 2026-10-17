# Review — Codex qwen, 2026-10-17

Branch `review/qwen`, one commit per document, nothing pushed. All findings are
`Review (Codex qwen, 2026-10-17):` blocks added next to the text they concern; no existing
text was deleted or rewritten. Source tags `[code] [docs] [calc] [bench] [owner]` are kept; an
untagged claim is my inference.

## What holds up

**The arithmetic is correct.** I re-derived every time-on-air and capacity figure from the Semtech
formula with exact rationals, independently of `tools/airtime.py` and `tools/cross_check.py`, using
the driver at `../upstream/sx126x-driver` (`a10c5df`, `src/sx126x.c`). All of them reproduce: 24 B
pre8 = 30.848 / 56.576 / 102.912 ms at SF7/SF8/SF9/250 and 370.688 ms at SF10/125; slot caps
19.58 / 12.02 / 7.09 per second; 20-rider frames 1.021 / 1.664 / 2.823 s; the SF10/125 route fit of
110.8 min. [calc]

**The DMD coordinate scales are right, not a typo.** docs/04's asymmetric 2.39 m north–south versus
4.78 m east–west looks like an error and is not: the fields use different scales,
`u24 = (lat+90) × 46603.375` and `(lon+180) × 23301.6875` (`dmd2-lora-bridge` `2234b5a`,
`docs/PROTOCOL.md:92-93`). [code]

**Meshtastic's `EU_866` frequencies are legal.** The four channel centres come out at 865.7, 866.3,
866.9, 867.5 MHz (`RadioInterface.cpp:92,1391,1349`), each 125 kHz allocation inside one row-47b
window with 37.5 kHz to spare. The reason `EU_866` is still wrong for a convoy is the missing
Adaptive Power Control (row 47b makes APC mandatory; no APC exists anywhere in `develop` at
`fad52bb`) and the 2.5 % mobile cap. [code, docs]

## Main findings

- **Half-duty gating does not cover positions** (docs/02). `airtime.cpp:319-335` throttles at
  `polite_duty_cycle_percent = 50` (`airtime.h:232`) and is called by nodeinfo, telemetry and
  beacon modules — but not by `PositionModule.cpp`, whose only gate is channel load (`:578`).
  Positions stop only at the hard abort in `Router.cpp:485-510`, at the **full** duty cycle. A
  companion on the node's BLE API will see telemetry go quiet long before positions do. [code]
- **`use_aead` is not a free upgrade** (docs/03). `encryptPacketCCM` builds the CCM nonce from
  `initNonce(fromNode, packetId)` with no extra nonce (`CryptoEngine.cpp:578-593`), and packet ids
  are only 22 bits of randomness (`Router.cpp:347-355`). At one position per 5 s that is roughly
  0.07 expected nonce repeats per rider-day, approaching a certainty across 20 riders — and under
  CCM a repeat breaks the forgery guarantee, not just confidentiality. Run the E6 signing
  comparison on the CTR PSK. [code, calc]
- **The 10 ms guard is a guess worth 24 % of capacity** (docs/06). Clock drift over a 1.2 s frame is
  48 µs, some 200× less than the guard; the real budget is PPS alignment error plus RX wake and
  start-transmit latency. A 2 ms guard lifts SF7/250 from 19.58 to 24.35 positions/s. Measure both
  components per board (added to E6/07). [calc]
- **The 24-byte sketch sums to 17 + tag and has no session field** (docs/06), although the security
  rules require a session in the nonce and the open questions ask how a receiver learns it after a
  reboot. Decide this before the length is fixed. Also: implicit header saves 8–10 % at 250 kHz and
  **exactly 0 % at SF10/125**, which is the mode `plan()` recommends for small long-range groups. [calc]
- **Route transfer dominates the long-range frame** (docs/06). One 192 B route packet is 4.70
  position slots, 23.5 % of a 20-rider SF7/250 frame — and at SF10/125 it is **52.7 % of the whole
  frame**. Phone-to-phone handover before the ride is the only workable answer, not advice. [calc]
- **Range is a property of the band, not the mode** (docs/02). Every budget is quoted at 22 dBm,
  which is neither permitted ERP. At the legal 27 dBm ERP (869.4–869.65) SF9/250 reaches 8.0 km; at
  14 dBm ERP (868.0–868.6) only 3.8 km. Quote range per mode *and* sub-band. [calc]
- **Trails need `TRACKS_EDIT`, not just `MAP`** (docs/05). MeshAnd draws trails through `importGpx`
  (`OsmAndBridge.kt:591,611`), and `LOCATION` is also not granted by default. A setup flow must ask
  for three groups at the moment the feature is used, or trails silently vanish. [code]
- **Four requirements are missing from docs/01**: a per-device duty ledger the device can export, a
  numeric freshness target for "help acknowledged", behaviour with no GPS fix at all, and
  interoperability stated as a requirement or a non-goal rather than a question. Plus two blocking
  questions: which countries, and whether the organiser carries a radio. [inference]
- **Two real costs are absent from docs/06**: RED 2014/53/EU conformity for a self-built radio (the
  docs cover spectrum *usage* limits only), and GDPR obligations for logged rider tracks. Neither
  changes the protocol; both belong in the cost of "build it ourselves". [docs, inference]
- **E6 cannot currently fail** (docs/07): no numeric pass criteria, no signed-vs-unsigned variant,
  no guard-time measurement, and the hidden-transmitter case should be mandatory rather than a
  variant, since that geometry is what decides whether flooding holds a column together. [inference]

## What I did not check

No radio, phone or ride tests — E1–E6 are all unrun, so every delivery and range statement here is
model or source, not measurement. No regulatory conformance test was performed; the RED and GDPR
paragraphs are flags, not legal advice. MeshAnd and vendor behaviour comes from their own docs and
source, not from running them. The AEAD nonce figure is an order-of-magnitude estimate assuming
uniform 22-bit randomness, not measured. `lab/` was left untouched and nothing about DMD2 4.036
internals is recorded here. BeeLine and AirVu came up in the alternatives search and were
deliberately **not** added: no primary source could be resolved for either.

## Reproducing the checks

Local mirrors of every pinned project are in `../upstream/` at the SHAs in docs/03's pin table, so
any `[code]` claim can be checked offline. `python3 -I tools/cross_check.py` and
`python3 tools/airtime.py` both pass on this branch.

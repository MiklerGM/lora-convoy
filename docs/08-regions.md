# 08. Regions: where these radios may transmit

A group that crosses borders meets a different rule set in each country ([01](01-goals.md),
question 6). This document collects what the rules say for the bands an SX1262 or SX1280 radio can
use without a licence, with the place in each source. It is information for the design, not legal
advice. Where no primary source has been found, it says so instead of guessing; the documents still
to find are listed in the working log.

Sources were read on 2026-10-17 and 18 and are listed at the end. Power is ERP unless marked.

## What a design needs from each country

- **Band edges**, and for some bands the allowed channels.
- **Power, and how it is measured:** ERP (against a dipole), EIRP (against an isotropic antenna)
  or conducted at the radio. Antenna gain counts in the first two ([02](02-radio-basics.md)).
- **The access rule:** a duty cycle; or polite spectrum access (PSA: listen before talk with
  adaptive frequency agility); or carrier sense with limits on transmission time (Japan); or
  frequency hopping (US, Australia).
- **Bandwidth:** caps on occupied bandwidth, or minimum bandwidths for higher power.
- **Restrictions:** data networks only, metering only, indoors only.

Two rules hold everywhere below:

- **A duty cycle limits each device, not the group** ([02](02-radio-basics.md)). The group is
  limited by channel load: Meshtastic withholds positions above 25 % of the airtime a node hears.
- **Polite spectrum access has limits of its own:** at most 1 s per transmission, 100 s an hour
  per 200 kHz of spectrum, and 100 ms off on the same frequency (EN 300 220-1 V3.1.1, Table 48)
  [docs]. A radio on one channel in 869.40–869.65 MHz therefore gets about 100–125 s an hour,
  under 3.5 %, less than the 10 % duty cycle there. PSA gains only in the 1 % and 0.1 % sub-bands.

## Europe

### EU and EEA

Commission Implementing Decision (EU) 2025/105, Annex, row numbers as printed [docs]:

| Band, MHz | Power | Access | Other conditions | Row |
|---|---|---|---|---|
| 169.400–169.475 | 500 mW | duty ≤ 1 % (≤ 10 % for metering devices) | occupied bandwidth ≤ 50 kHz (EN 300 220-2, Table B.1, row D) | 37c |
| 433.05–434.79 | 1 mW | — | | 44a |
| 433.05–434.79 | 10 mW | duty ≤ 10 % | | 44b |
| 434.04–434.79 | 10 mW | duty ≤ 100 % | bandwidth ≤ 25 kHz | 45c |
| 863–865 | 25 mW | PSA, or duty ≤ 0.1 % | | 46a |
| 865–868 | 25 mW | PSA, or duty ≤ 1 % | | 47 |
| 865.6–865.8, 866.2–866.4, 866.8–867.0, 867.4–867.6 | 500 mW | adaptive power control; duty ≤ 10 % for fixed network access points, ≤ 2.5 % otherwise | bandwidth ≤ 200 kHz; data networks only | 47b |
| 868.0–868.6 | 25 mW | PSA, or duty ≤ 1 % | | 48 |
| 868.7–869.2 | 25 mW | PSA, or duty ≤ 0.1 % | | 50 |
| **869.40–869.65** | **500 mW** | **PSA, or duty ≤ 10 %** | | **54** |
| 869.7–870.0 | 5 mW | — | | 56a |
| 869.7–870.0 | 25 mW | PSA, or duty ≤ 1 % | | 56b |
| 2400–2483.5 | 10 mW EIRP | — | non-specific devices | 57a |
| 2400–2483.5 | 100 mW EIRP | access techniques (EN 300 328) | wideband data; 100 mW/100 kHz when hopping, 10 mW/MHz otherwise | 57c |

The harmonised standard EN 300 220-2 V3.3.1 (Annex B) maps these rows into its own table; it is
informative, and the Decision governs.

### The rest of Europe

| Country | Source | 869.40–869.65 MHz | Also checked |
|---|---|---|---|
| United Kingdom | Ofcom IR2030, v1.15 (2023) | 500 mW, access techniques or duty ≤ 10 % (IR2030/1/19) | 169.4–169.475 MHz: 500 mW, ≤ 50 kHz, duty ≤ 1 % (/1/41); 433 MHz: 10 mW, duty ≤ 10 % (/1/10); the 865.6–867.6 MHz windows are 500 mW **EIRP**, data networks only (/1/46) |
| Norway | Nkom's table, an unofficial guide to regulation FOR-2012-01-19-77, which prevails | 500 mW (§ 8(19)) | |
| Turkey | BTK technical criteria for licence-exempt equipment, annex 5 | 500 mW, duty ≤ 10 % or LBT+AFA (row 21) | |
| Serbia | RATEL rulebook on general authorisation, 2024 | ≤ 500 mW, duty < 10 % (g3) | |
| Albania | AKEP national frequency plan 2025 | numbers deferred to ERC/REC 70-03 | the plan lists 862–876 MHz and 919.4–925 MHz as free use, without limits |
| Switzerland, Bosnia and Herzegovina, Montenegro, North Macedonia, Kosovo, Georgia, Armenia | **no current primary source** | — | the only implementation matrix found is ERC/REC 70-03 of 2011; it marks the 863–870 MHz annex as not implemented in Georgia, and has no column for Armenia or Kosovo |

## Africa

| Country | Source | 869.40–869.65 MHz | Also checked |
|---|---|---|---|
| Morocco | ANRT decision on free-use radio equipment, as amended 7 May 2021, Annex | **500 mW, no duty limit printed** (I.9) | 869.0–869.4 MHz: 25 mW (I.8); 433.05–434.79 MHz: 10 mW, duty ≤ 10 % (I.6); 169.4–169.8125 MHz: only 10 mW (I.4); 2.4 GHz: 10 mW EIRP (I.11), 100 mW EIRP indoors only (V.1) |
| Tunisia | **no source** | — | |

ANRT's national frequency plan of 2021 lists the 863–870 MHz sub-bands as free use and defers the
limits to such decisions.

## Central Asia and Mongolia

- **Kazakhstan:** a government page reportedly allows 863–868 MHz for IoT at 25 mW and 500 kHz,
  excluding 869.525 MHz, where Meshtastic's EU_868 transmits. We could not read it: the page is a
  JavaScript application. Meshtastic's `KZ_433` and `KZ_863` regions cite only a GitHub issue
  [code: `RadioInterface.cpp:268-269` at `fad52bb`].
- **Kyrgyzstan, Uzbekistan, Tajikistan, Mongolia:** no source.

## Reference regions

**United States**, 47 CFR 15.247 [docs]:

- 902–928 MHz with digital modulation: 1 W conducted, if the 6 dB bandwidth is at least 500 kHz;
- with frequency hopping: at least 50 channels and 0.4 s per 20 s when a channel's 20 dB
  bandwidth is under 250 kHz; at least 25 channels and 0.4 s per 10 s from 250 to 500 kHz, with
  0.25 W for 25–49 channels and 1 W from 50;
- antennas above 6 dBi reduce the allowed power; coordinating hopping transmitters so they avoid
  each other is not permitted (§ 15.247(h)), which matters for a schedule that hops.

A single 250 kHz LoRa channel at 1 W, Meshtastic's US default, fits neither route; its wide
presets (812.5 kHz) clear the 500 kHz floor [code, inference]. Canada applies RSS-247 to the
same bands with its own certification; Brazil allows 902–907.5 MHz and 915–928 MHz (ANATEL act
14448) [docs].

**Australia**, LIPD Class Licence 2025, in force since 1 October 2025 [docs]:

- 915–928 MHz: frequency hopping up to 1 W (item 1, clause 39); digital modulation up to 1 W
  with radiated peak power density ≤ 25 mW per 3 kHz (item 6, clause 43); anything else 3 mW
  (item 20). The 500 kHz minimum of the 2000 licence is gone.
- At 1 W, a 250 kHz LoRa signal carries about 12 mW per 3 kHz and a 125 kHz signal about 24 mW,
  within the cap; a 62.5 kHz signal, about 48 mW, is not [calc, assuming a flat spectrum].

**Japan**, ARIB STD-T108 v1.5 (2023) [docs]:

- Licence-free use is Part 2, specified low-power radio stations: 915.9–916.9 and
  920.5–929.7 MHz, 20 mW or less (1 mW on some channels), EIRP up to 16 dBm, 200 kHz unit
  channels.
- Carrier sense before every transmission (128 µs or 5 ms, depending on the channel), a
  transmission lasting at most 4 s and then a pause; some channels also cap the total
  transmission time per hour.
- Part 1, 250 mW and up to 27 dBm EIRP, covers land mobile stations, which need a licence or
  registration.

Meshtastic's `JP` region (920.5–923.5 MHz, 13 dBm) matches the licence-free power; whether its
check of the channel before sending meets the carrier-sense rule is untested [code, inference].

## Meshtastic's regions against these rules

| Region | Meshtastic [code] | Against the sources |
|---|---|---|
| `EU_868` | 869.40–869.65 MHz, 10 %, 27 dBm cap on conducted power | inside the 869.40–869.65 MHz rules of the EU, UK, Norway, Turkey, Serbia and Morocco; the law counts radiated power |
| `EU_866` (`develop` only) | four channels at 865.7–867.5 MHz | inside row 47b's windows, but adaptive power control is required and the windows are for data networks ([02](02-radio-basics.md)) |
| `KZ_863` | 863–868 MHz, 30 dBm | the reported national limit is 25 mW |
| `US` | 902–928 MHz, 30 dBm | the 250 kHz default fits neither route of § 15.247 |
| `ANZ` | 915–928 MHz, 30 dBm | within the 2025 power-density cap at 125–250 kHz, not at 62.5 kHz |
| `JP` | 920.5–923.5 MHz, 13 dBm | licence-free power; carrier sense untested |

Riders in the countries without a source, and in Switzerland and most of the Balkans, would pick
`EU_868` out of habit.

## What this means for the design

1. **Region profiles, each with a dated source:** band, power as ERP or EIRP including the
   antenna's gain, the access rule and the bandwidth. A country without a verified source is
   flagged; whether a radio then stays silent or leaves it to the rider is the owner's call.
2. **869.40–869.65 MHz at 500 mW ERP and 10 % is the one band confirmed along the route so far**
   (EU, UK, Norway, Turkey, Serbia, Morocco). It is the design band for both paths; it is shared
   with public Meshtastic traffic and LoRaWAN downlinks at 869.525 MHz.
3. **There, the duty cycle is the better route.** Polite spectrum access gives less, and its 1 s
   cap per transmission and 100 ms off-time would constrain a schedule anyway.
4. **One PHY profile cannot serve every region.** Japan needs carrier sense before every
   transmission, a scheduled slot included; the US needs 500 kHz or hopping for full power;
   Australia's density cap allows 125–250 kHz at 1 W.
5. **Hardware differs by region:** 868, 915 and 920 MHz boards and antennas are not
   interchangeable.
6. **Power is the rider's antenna as much as the radio.** Firmware caps conducted power, while the
   law counts radiated power. A 22 dBm SX1262 into a dipole is 22 dBm ERP: legal in
   869.40–869.65 MHz, 8 dB over the 25 mW sub-bands elsewhere in 863–870 MHz.

## Sources

| Document | Link |
|---|---|
| Commission Implementing Decision (EU) 2025/105 | <https://eur-lex.europa.eu/eli/dec_impl/2025/105/oj/eng/pdf> |
| ETSI EN 300 220-1 V3.1.1 | <https://www.etsi.org/deliver/etsi_en/300200_300299/30022001/03.01.01_60/en_30022001v030101p.pdf> |
| ETSI EN 300 220-2 V3.3.1 | <https://www.etsi.org/deliver/etsi_en/300200_300299/30022002/03.03.01_60/en_30022002v030301p.pdf> |
| ETSI EN 300 328 V2.2.2 | <https://www.etsi.org/deliver/etsi_en/300300_300399/300328/02.02.02_60/en_300328v020202p.pdf> |
| Ofcom IR2030 v1.15 (archived copy) | <https://web.archive.org/web/20240618044113/https://www.ofcom.org.uk/siteassets/resources/documents/spectrum/interface-requirements/ir-2030.pdf> |
| Nkom, table of licence-free equipment (Norway) | nkom.no; unofficial guide to FOR-2012-01-19-77 |
| BTK technical criteria, annex 5 (Turkey) | <https://www.btk.gov.tr/uploads/pages/ftm-teknik-olcutler-ek-5.pdf> |
| RATEL rulebook on general authorisation (Serbia) | <https://www.ratel.rs/storage/upload/2024/09/Rulebook%20on%20the%20manner%20of%20radio%20frequency%20usage%20under%20general%20authorization%20regime.pdf> |
| AKEP national frequency plan 2025 (Albania) | <https://akep.al/wp-content/uploads/2025/01/PKF_2025-1.pdf> |
| ANRT decision on free-use equipment, 2021 (Morocco) | <https://www.anrt.ma/sites/default/files/decision_a2fp_-vf-_mod_07.05.2021.pdf> |
| ANRT national frequency plan 2021 (Morocco) | <https://www.anrt.ma/sites/default/files/document/pnf-2021_0.pdf> |
| 47 CFR Part 15, Subpart C (US) | <https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-C/section-15.247> |
| Canada Gazette notice on RSS-247 Issue 4 | <https://gazette.gc.ca/rp-pr/p1/2025/2025-08-09/html/notice-avis-eng.html> |
| ANATEL act 14448 (Brazil) | <https://informacoes.anatel.gov.br/legislacao/atos-de-certificacao-de-produtos/2017/1139-ato-14448> |
| ACMA LIPD Class Licence 2025 (Australia) | <https://www.legislation.gov.au/Details/F2025L01047> |
| ARIB STD-T108 v1.5 (Japan) | <https://www.arib.or.jp/english/html/overview/doc/5-STD-T108v1_5-E1.pdf> |
| ERC/REC 70-03, edition of 2011 (superseded; the current edition could not be downloaded) | docdb.cept.org |

Collected by the Qwen and Codex agents and checked against the documents by the main agent
(2026-10-18).

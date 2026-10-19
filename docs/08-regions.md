# 08. Regions: where these radios may transmit

A group that crosses borders meets a different rule set in each country ([01](01-goals.md),
question 6). This document collects what the rules say for the bands an SX1262 or SX1280 radio can
use without a licence, with the place in each source. It is information for the design, not legal
advice. Where no primary source has been found, it says so instead of guessing; the documents still
to find are listed in the working log.

Sources were read between 2026-10-17 and 19 and are listed at the end. Power is ERP unless marked.

## What a design needs from each country

- **Band edges**, and for some bands the allowed channels.
- **Power, and how it is measured:** ERP (against a dipole), EIRP (against an isotropic antenna)
  or conducted at the radio. Antenna gain counts in the first two ([02](02-radio-basics.md)).
- **The access rule:** a duty cycle; or polite spectrum access (PSA: listen before talk with
  adaptive frequency agility, LBT+AFA); or carrier sense with limits on transmission time
  (Japan); or frequency hopping (US, Australia).
- **Bandwidth:** caps on occupied bandwidth, or minimum bandwidths for higher power.
- **Restrictions:** data networks or their operators only, metering only, indoors only, downlink
  only, certified equipment only.

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

### CEPT

ERC/REC 70-03, edition of June 2026, Annex 1, band h1.7: 869.40–869.65 MHz, 500 mW, duty ≤ 10 %
or LBT+AFA. Its national implementation matrix lists h1.7 as implemented by Albania, Bosnia and
Herzegovina, Georgia, Montenegro, North Macedonia, Norway, Serbia, Switzerland and Turkey, and as
not implemented by Ukraine and Azerbaijan [docs]. The 2011 edition used earlier had Georgia as
not implementing the 863–870 MHz annex; that is superseded.

### The rest of Europe and the Caucasus

| Country | Source | 869.40–869.65 MHz | Notes |
|---|---|---|---|
| United Kingdom | Ofcom IR2030 v1.16 (April 2026) | 500 mW, access techniques or duty ≤ 10 % (IR2030/1/19) | unchanged from v1.15, as are 169.4 MHz at 500 mW, ≤ 1 % (/1/41) and 433 MHz at 10 mW, ≤ 10 % (/1/10). The 865–868 MHz rows of v1.15 (/1/14, and /1/46: four windows at 500 mW **EIRP**, data networks only) did not come through our copy of v1.16 |
| Norway | regulation FOR-2012-01-19-77, § 8(19) | 500 mW, transmit time ≤ 10 %; more with interference mitigation at least as effective as the harmonised standards' | no bandwidth cap; Nkom's table is an unofficial guide to the regulation |
| Switzerland | OFCOM, RIR1008-09 (edition 7.0, 2026) | 500 mW, duty ≤ 10 % or LBT+AFA | |
| Turkey | BTK technical criteria, annex 5 | 500 mW, duty ≤ 10 % or LBT+AFA (row 21) | |
| Serbia | RATEL rulebook on general authorisation (2024) | ≤ 500 mW, duty < 10 % (g3) | |
| Bosnia and Herzegovina | RAK rule 81/2017 | 500 mW (row 23); duty and access by reference to ERC/REC 70-03 and EU rules | the whole band may be used as one channel |
| Montenegro | EKIP rulebook 77/25 | 500 mW, duty < 10 % or LBT+AFA (h1.6) | |
| North Macedonia | AEK rulebook 137/2026 | 500 mW, duty < 10 % or LBT+AFA | |
| Albania | AKEP regulation 33/2014, still the one AKEP publishes | ≤ 500 mW, duty ≤ 10 % or a suitable access protocol; channels of 25 kHz, or the whole band as one data channel | no later version found; the 2025 frequency plan lists 862–876 and 919.4–925 MHz as free use and defers the limits |
| Kosovo | regulation of ART, now ARKEP, on radio stations that need no licence (2010), in force until replaced | 500 mW, duty < 10 %, channel width 25 kHz | the CEPT table of the time gave 25 kHz as channel spacing and let the whole band serve as one channel; Kosovo's copy omits that, so whether 125–250 kHz LoRa fits is **unclear**. A later regulation (No. 40, 2018) is listed in web archives but could not be read; the 2024 frequency plan has no rows for these devices |
| Georgia | GNCC requirements for short-range devices, published as a **consultation draft**; adoption not found | 500 mW, duty ≤ 10 % or LBT+AFA | ERC/REC 70-03 (2026) lists the band as implemented |
| Armenia | PSRC decision 169-N, consolidated edition of December 2024 | up to 500 mW, but for **"downlink transmissions"** only; no duty cycle printed | the general row is 863–870 MHz at 25 mW, and 868.0–868.6 MHz is marked "uplink". A mesh between riders is neither: until clarified, assume 25 mW |

## Africa

| Country | Source | 869.40–869.65 MHz | Also checked |
|---|---|---|---|
| Morocco | ANRT decision on free-use radio equipment, as amended 7 May 2021, Annex | 500 mW, no duty limit printed (I.9) | 869.0–869.4 MHz: 25 mW (I.8); 433.05–434.79 MHz: 10 mW, duty ≤ 10 % (I.6); 169.4–169.8125 MHz: only 10 mW (I.4); 2.4 GHz: 10 mW EIRP (I.11), 100 mW EIRP indoors only (V.1) |
| Tunisia | ministry order of 3 March 2023 on short-range devices, published by ANF | 500 mW, duty ≤ 10 %, in the rows for **operators of IoT access networks** | other devices: 868–869 MHz at 25 mW, no duty printed. Whether a rider's radio may use the operators' rows is **unclear**. The annex is a scan, read by eye; an order of 2024 changes only Wi-Fi at 6 GHz |

ANRT's national frequency plan of 2021 lists the 863–870 MHz sub-bands as free use and defers the
limits to such decisions.

## Central Asia and Mongolia

- **Kazakhstan**, ministry order No. 34 of 2015 as amended in 2022, the list of radio equipment
  usable without a permit [docs]:
  - 433.04–434.79 MHz: 10 mW EIRP, channels ≤ 25 kHz;
  - 863–868 MHz: 25 mW **EIRP**, channels ≤ 100 kHz, duty ≤ 0.1 % or LBT+AFA. A 125 kHz LoRa
    signal is over the cap, a 62.5 kHz one fits;
  - the same band for IoT devices (LoRaWAN is named): 25 mW, up to 500 kHz, not within airports,
    no duty printed. Whether a rider's radio counts as one is unclear;
  - **nothing above 868 MHz**: 869.40–869.65 MHz, where Meshtastic's `EU_868` transmits, is not
    on the list.

  Meshtastic's `KZ_863` allows 30 dBm, although its own comment says 25 mW EIRP; it cites only a
  GitHub issue. Both `KZ_433` and `KZ_863` offer only presets of 125 kHz and wider [code:
  `RadioInterface.cpp:42-44,262-269` at `fad52bb`]. The national frequency table was not obtained.
- **Kyrgyzstan, Uzbekistan, Tajikistan, Mongolia:** no source; the regulators' sites were
  unreachable or held no such document.

## Reference regions

**United States**, 47 CFR 15.247 [docs]:

- 902–928 MHz with digital modulation: 1 W conducted, if the 6 dB bandwidth is at least 500 kHz;
- with frequency hopping: at least 50 channels and 0.4 s per 20 s when a channel's 20 dB
  bandwidth is under 250 kHz; at least 25 channels and 0.4 s per 10 s from 250 to 500 kHz, with
  0.25 W for 25–49 channels and 1 W from 50;
- antennas above 6 dBi reduce the allowed power; coordinating hopping transmitters so they avoid
  each other is not permitted (§ 15.247(h)), which matters for a schedule that hops.

A single 250 kHz LoRa channel at 1 W, Meshtastic's US default, fits neither route. Its Turbo
presets (500 kHz) sit at the 500 kHz floor, and whether their 6 dB bandwidth reaches it is for
measurement; the 812.5 kHz presets belong to 2.4 GHz only [code: `RadioInterface.cpp:101,120,354`,
`MeshRadio.h:221-251` at `fad52bb`; inference]. Canada applies the same structure
in RSS-247 Issue 4 with its own certification; Brazil allows 902–907.5 MHz and 915–928 MHz
(ANATEL act 14448) [docs].

**Australia**, LIPD Class Licence 2025, in force since 1 October 2025 [docs]:

- 915–928 MHz: frequency hopping up to 1 W (item 1, clause 39); digital modulation up to 1 W
  with radiated peak power density ≤ 25 mW per 3 kHz (item 6, clause 43); anything else 3 mW
  (item 20). The 500 kHz minimum of the 2000 licence is gone.
- At 1 W, a 250 kHz LoRa signal carries about 12 mW per 3 kHz and a 125 kHz signal about 24 mW,
  within the cap; a 62.5 kHz signal, about 48 mW, is not [calc, assuming a flat spectrum].

**Japan** [docs]:

- Licence-free use is the specified low-power radio station. The Radio Act exempts it only with
  **certified equipment** (art. 4 no. 3); the Enforcement Rules define it, for telemetry,
  telecontrol and data in 915–930 MHz (art. 6(4)(2)); the Radio Equipment Regulations set its
  limits (art. 49-14, items 7 and 8).
- 920.5–928.1 MHz: 20 mW, EIRP up to 16 dBm with an antenna of at most 3 dBi, on 200 kHz unit
  channels centred at 920.6 MHz plus multiples of 200 kHz; a 1 mW class covers 915.9–929.7 MHz.
  The 20 mW is from ARIB STD-T108 v1.5, Part 2: the ministry's notifications that set the numbers
  were not obtained.
- Access is one of three:
  - carrier sense and a limit on transmit time: after 5 ms of sensing, up to 4 s per
    transmission on channels centred at 920.6–923.4 MHz; after 128 µs, up to 400 ms per
    transmission and 360 s an hour per channel, at 922.4–928.0 MHz;
  - hopping at least every 0.4 s, in 920.5–925.1 MHz;
  - no carrier sense, if the radio transmits at most 1 % of any hour (36 s), in
    920.5–923.5 MHz.
- The 250 mW class (ARIB Part 1, up to 27 dBm EIRP, 920.5–923.5 MHz) is a land mobile station,
  which needs a licence or registration (Radio Equipment Regulations, art. 49-34).

Meshtastic's `JP` region (920.5–923.5 MHz, 13 dBm) matches the licence-free power. It keeps no
budget per hour, and its channels step from the band edge by the preset's width, off the 200 kHz
grid; whether its check of the channel before sending meets the carrier-sense rule is untested
[code, inference].

## Meshtastic's regions against these rules

| Region | Meshtastic [code] | Against the sources |
|---|---|---|
| `EU_868` | 869.40–869.65 MHz, 10 %, 27 dBm cap on conducted power | inside the rules of the EU, UK, Norway, Switzerland, Turkey, Serbia, Bosnia, Montenegro, North Macedonia, Albania, Morocco and Georgia's draft; unclear in Armenia, Tunisia and Kosovo; not on Kazakhstan's list. The law counts radiated power |
| `EU_866` (`develop` only) | four channels at 865.7–867.5 MHz | inside row 47b's windows, but adaptive power control is required and the windows are for data networks ([02](02-radio-basics.md)) |
| `KZ_863` | 863–868 MHz, 30 dBm, presets of 125 kHz and wider | Kazakhstan allows 25 mW EIRP and channels up to 100 kHz (500 kHz for IoT devices) |
| `KZ_433` | 433.075–434.775 MHz, 10 dBm, the same presets | the power fits; Kazakhstan's channels are at most 25 kHz |
| `US` | 902–928 MHz, 30 dBm | the 250 kHz default fits neither route of § 15.247 |
| `ANZ` | 915–928 MHz, 30 dBm | within the 2025 power-density cap at 125–250 kHz, not at 62.5 kHz |
| `JP` | 920.5–923.5 MHz, 13 dBm | licence-free power; no hourly budget, channels off the 200 kHz grid, carrier sense untested |

Riders in a country without a region of its own pick `EU_868` out of habit.

## What this means for the design

1. **Region profiles, each with a dated source:** band, power as ERP or EIRP including the
   antenna's gain, the access rule and the bandwidth. A country without a verified source is
   flagged; whether a radio then stays silent or leaves it to the rider is the owner's call.
2. **869.40–869.65 MHz at 500 mW ERP and 10 % holds along most likely routes:** the EU, UK,
   Norway, Switzerland, Turkey, Serbia, Bosnia, Montenegro, North Macedonia, Albania, Georgia (by
   draft and by the CEPT matrix) and Morocco. It is the design band for both paths. It is shared
   with public Meshtastic traffic and LoRaWAN downlinks at 869.525 MHz.
3. **The exceptions need profiles of their own:** Kazakhstan (863–868 MHz at 25 mW EIRP, channels
   up to 100 kHz or the IoT class, 0.1 % or LBT+AFA); Armenia and Tunisia, where 500 mW belongs to
   a role a rider's radio does not have, downlink or network operator, so 25 mW until clarified;
   Kosovo, until its 25 kHz column is; and no profile yet for Kyrgyzstan, Uzbekistan, Tajikistan
   and Mongolia.
4. **In 869.40–869.65 MHz the duty cycle is the better route.** Polite spectrum access gives less
   there, and its 1 s cap per transmission and 100 ms off-time would constrain a schedule anyway.
5. **One PHY profile cannot serve every region.** Japan needs carrier sense before every
   transmission, a scheduled slot included, unless a radio stays under 1 % of the hour or hops;
   the US needs 500 kHz or hopping for full power; Australia's density cap allows 125–250 kHz at
   1 W; Kazakhstan caps channels at 100 kHz outside its IoT class.
6. **Hardware differs by region:** 868, 915 and 920 MHz boards and antennas are not
   interchangeable, and Japan exempts only radios certified there.
7. **Power is the rider's antenna as much as the radio.** Firmware caps conducted power, while the
   law counts radiated power. A 22 dBm SX1262 into a dipole is 22 dBm ERP: legal in
   869.40–869.65 MHz, 8 dB over the 25 mW sub-bands elsewhere in 863–870 MHz.

## Sources

| Document | Link |
|---|---|
| Commission Implementing Decision (EU) 2025/105 | <https://eur-lex.europa.eu/eli/dec_impl/2025/105/oj/eng/pdf> |
| ETSI EN 300 220-1 V3.1.1 | <https://www.etsi.org/deliver/etsi_en/300200_300299/30022001/03.01.01_60/en_30022001v030101p.pdf> |
| ETSI EN 300 220-2 V3.3.1 | <https://www.etsi.org/deliver/etsi_en/300200_300299/30022002/03.03.01_60/en_30022002v030301p.pdf> |
| ETSI EN 300 328 V2.2.2 | <https://www.etsi.org/deliver/etsi_en/300300_300399/300328/02.02.02_60/en_300328v020202p.pdf> |
| ERC/REC 70-03, edition of June 2026 (via EFIS) | <https://efis.cept.org/reports/ReportDownloader?reportid=2> |
| ERC/REC 70-03, edition of 2011 (superseded) | docdb.cept.org; the download link was not recorded |
| Ofcom IR2030 v1.16, 2026 (archived copy) | <https://web.archive.org/web/20260531222955if_/https://www.ofcom.org.uk/siteassets/resources/documents/spectrum/interface-requirements/ir-2030.pdf?v=415472> |
| Ofcom IR2030 v1.15, 2023 (archived copy) | <https://web.archive.org/web/20240618044113/https://www.ofcom.org.uk/siteassets/resources/documents/spectrum/interface-requirements/ir-2030.pdf> |
| Norway, regulation FOR-2012-01-19-77 | <https://lovdata.no/dokument/SF/forskrift/2012-01-19-77> |
| Switzerland, OFCOM RIR1008-09 | <https://www.ofcomnet.ch/api/RIR/1008/09> |
| Turkey, BTK technical criteria, annex 5 | <https://www.btk.gov.tr/uploads/pages/ftm-teknik-olcutler-ek-5.pdf> |
| Serbia, RATEL rulebook on general authorisation | <https://www.ratel.rs/storage/upload/2024/09/Rulebook%20on%20the%20manner%20of%20radio%20frequency%20usage%20under%20general%20authorization%20regime.pdf> |
| Bosnia and Herzegovina, RAK rule 81/2017 | <https://www.sluzbenilist.ba/page/akt/tAb1DAP1Jg8%3D> |
| Montenegro, EKIP rulebook 77/25 | ekip.me (Sl. list CG 077/25) |
| North Macedonia, AEK rulebook 137/2026 | aek.mk (2026 upload) |
| Albania, AKEP regulation 33/2014 | <https://akep.al/wp-content/uploads/2021/07/RREGULLORE_PER_KUSHTET_E_PERDORIMIT_TE_FREKUENCAVE_PA_AUTORIZIM_INDIVIDUAL-120914.pdf> |
| Albania, AKEP national frequency plan 2025 | <https://akep.al/wp-content/uploads/2025/01/PKF_2025-1.pdf> |
| Kosovo, ART regulation on radio stations that need no licence, 2010 (archived copy) | <https://web.archive.org/web/20190923113258/http://arkep-rks.org/repository/docs/Rregullorja%20%20per%20Radio%20Stacione%20per%20te%20cilat%20nuk%20kerkohet%20License%20per%20shfrytezimin%20e%20Radio%20Frekuencave.pdf> |
| Kosovo, ARKEP national frequency allocation plan 2024 | <https://www.arkep-rks.org/desk/inc/media/9E415630-271A-41D6-9B66-FF4A71227474.pdf> |
| Georgia, GNCC interface requirements (consultation draft) | <https://comcom.ge/uploads/other/3/3261.pdf> |
| Armenia, PSRC decision 169-N (consolidated) | <https://arlis.am/hy/acts/85612/latest> |
| Morocco, ANRT decision on free-use equipment, 2021 | <https://www.anrt.ma/sites/default/files/decision_a2fp_-vf-_mod_07.05.2021.pdf> |
| Morocco, ANRT national frequency plan 2021 | <https://www.anrt.ma/sites/default/files/document/pnf-2021_0.pdf> |
| Tunisia, ministry orders on short-range devices, 2023 and 2024 (ANF) | <https://www.anf.tn/sites/default/files/2025-07/Arr%C3%AAt%C3%A9_SRD_2023_Arabe.pdf>, <https://www.anf.tn/sites/default/files/2025-07/Arr%C3%AAt%C3%A9_SRD_2024_Arabe.pdf> |
| Kazakhstan, ministry order No. 34 (as amended) | <https://zakon.uchet.kz/rus/docs/V2200026927> |
| US, 47 CFR 15.247 | <https://www.ecfr.gov/current/title-47/chapter-I/subchapter-A/part-15/subpart-C/section-15.247> |
| Canada, RSS-247 Issue 4 | <https://ised-isde.canada.ca/site/spectrum-management-telecommunications/en/devices-and-equipment/radio-equipment-standards/radio-standards-specifications-rss/rss-247-digital-transmission-systems-dtss-frequency-hopping-systems-fhss-and-licence-exempt-local> |
| Brazil, ANATEL act 14448 | <https://informacoes.anatel.gov.br/legislacao/atos-de-certificacao-de-produtos/2017/1139-ato-14448> |
| Australia, ACMA LIPD Class Licence 2025 | <https://www.legislation.gov.au/Details/F2025L01047> |
| Japan, Radio Act, Enforcement Rules and Radio Equipment Regulations (e-Gov) | <https://laws.e-gov.go.jp/api/2/law_data/325AC0000000131>, <https://laws.e-gov.go.jp/api/2/law_data/325M50080000014>, <https://laws.e-gov.go.jp/api/2/law_data/325M50080000018> |
| Japan, ARIB STD-T108 v1.5 | <https://www.arib.or.jp/english/html/overview/doc/5-STD-T108v1_5-E1.pdf> |

Collected by the Qwen and Codex agents and checked against the documents by the main agent
(2026-10-18 and 19).

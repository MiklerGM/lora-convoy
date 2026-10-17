# Research review — Codex

**Review (Codex, 2026-10-17):** [code] Reviewed README, docs/01–07 and both original tools
from `main` at `fe4c836018fe91683540dc30c5e24aca1e9ce1cb`. Existing text is preserved;
inline review blocks cite primary documentation, source paths and immutable commit SHAs.

- **Protocol security needs a different nonce design.** [docs][calc] Fix time repeats when
  GPS stalls and wraps after 18 h 12 min 16 s. Add a reboot-safe transmission identity/counter,
  replay handling and an explicit short-tag risk budget before fixing packet length
  ([06](docs/06-own-protocol.md), NIST/RFC citations inline).
- **Capacity is an offered-airtime budget.** [calc] Independent derivation confirms the basic
  57.984/190.976 ms examples. Twenty scheduled riders need 1.0212/1.6644/2.8228 s at SF7/8/9,
  before extra traffic; delivery and fix age still need measurement ([02](docs/02-radio-basics.md)).
- **The frame does not yet carry its other traffic.** [calc] At SF7/250, route and relay packets
  need 192.128/64.128 ms, exceeding a 40.848 ms position slot. Inference: define typed slots,
  unique assignment, authenticated synchronization and loss-of-clock behavior; budget every
  transmission per sender. Direct-only v1 does not solve a split column ([06](docs/06-own-protocol.md)).
- **Route safety is not demonstrated by the size tool.** [code][calc] Counterexamples show
  XML comments accepted as coordinates, prefixed GPX omitted, a 2 m simplification becoming
  2.449 m after rounding, and erased backtracking. Receiver-side snapping and route acceptance
  need explicit checks ([tools comments](tools/route_size.py), [06](docs/06-own-protocol.md)).
- **Pin the baseline before choosing a replacement.** [code][calc] Meshtastic 2.7.26 and main
  differ; signing changes the 56-byte comparison. MeshCore's current preamble makes the example
  508.928 ms; stock Sideband uses a collector with minute-scale telemetry. MeshAnd's freshness
  defaults are also minute-scale; OsmAnd needs enabled access and the correct AIDL version
  ([03](docs/03-alternatives.md), [05](docs/05-osmand-integration.md)).
  [code] MeshCore-Solo already offers optional rider relaying on the same radio; include it
  before rejecting MeshCore solely for its topology ([03](docs/03-alternatives.md)).
- **Regulatory profiles require more than a frequency choice.** [docs] Duty includes routes,
  relays and retries; moving routers do not establish EU_866 fixed-access-point eligibility.
  EU 169.4 MHz general SRDs have a 1% condition, and nominal channel widths do not establish
  compliant emissions ([02](docs/02-radio-basics.md), [06](docs/06-own-protocol.md), official sources inline).

Inference: test tuned Meshtastic plus MeshAnd first, with fix-age and help-delivery measurements.
Build the new protocol only against a demonstrated shortfall and after closing its security,
timing and route-transfer gaps.

Validation: `python3 -I tools/review_checks.py` passes independent rational airtime assertions,
20 comparisons against the original calculator and the GPX counterexamples. Original tool
self-checks pass; their executable logic is unchanged. Diff checks confirm additive edits only.
No radio, phone, ride or regulatory-conformance tests were performed. Historical ride results,
private GPX inputs and unmeasured vendor performance remain unverified; `lab/` was left alone.

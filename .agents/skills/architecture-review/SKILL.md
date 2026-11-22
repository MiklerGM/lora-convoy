---
name: architecture-review
description: Review changes to ownership, message semantics, or data paths across the air protocol, board, Android app, and OsmAnd. Use when planning or reviewing a contract change or simplifying a design.
---

# Architecture review

Start with what the rider should know or be able to do. Identify the component that can observe the fact firsthand and follow one concrete event through the actual producers and consumers. The [architecture decisions](../../../docs/ARCHITECTURE.md) describe responsibilities. Check the [protocol](../../../docs/PROTOCOL.md) and the implementation for what crosses each boundary today.

- For a position, trace the GNSS fix through the transmitting board, any relay, the receiving board's phone API, and the map. Track who measured it, when, and what survives a lost fix or a disconnected phone. A relay can know when it heard the sender even when the original fix time is unknown.
- For a command, identify exactly what each acknowledgement establishes. If the rider changes state twice and returns to the original value, can an echo of the first change be mistaken for receipt of the latest one?
- Before adding a field, store, or service, identify the decision it enables and compare it with the simplest path that preserves that decision. If the air format changes, check airtime and the remaining slot margin using the serialized packet size. For a proposed route server, trace what the group can still do offline and which data the server actually needs.
- Check which contract changes on the air, in an invitation, over the phone API, or in OsmAnd AIDL. The PoC permits coordinated breaking changes. Check that the change covers both ends and that a version mismatch is handled explicitly. For independently released consumers such as OsmAnd, check the versions the app supports.

For a finding, give the concrete scenario, code path, consequence, and smallest correction. A board can report a rider it knows only through a relay. Check the evidence at the map when the claim concerns a visible marker.

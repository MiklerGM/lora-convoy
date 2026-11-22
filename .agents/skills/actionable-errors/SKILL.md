---
name: actionable-errors
description: Design and review rider-facing errors, board replies, and bench diagnostics when a command, radio transmission, BLE link, or map update fails or only partly completes.
---

# Actionable errors

Find the last step known to have succeeded and the first one whose outcome is unknown or failed. Check the producer and the code that presents the result. A useful rider message identifies the affected board or action, explains the consequence for this ride, and gives the next step the product actually supports.

- A board can have a GNSS position while its clock is too uncertain to use its slot. If the board reports that reason, tell the rider that position sharing is paused. Its clock source, error estimate, and slot budget help the bench operator diagnose the same event.
- If Bluetooth drops while sending a rider state, check whether the command reached the board and what reconnect does with it. A timeout can leave the outcome unknown. Recommend a resend only when that outcome or the command's retry behavior makes it appropriate.
- Follow the message to its destination. When the board applies a setting but reports `kept=0`, the rider should know it works now and can be lost on restart. Verify that meaning survives the board reply and the Android display. For recurring conditions, report transitions and recovery when repeated messages would obscure them.
- Choose fields for the audience. A rider can recognize a roster name or a board label. A bench operator can correlate a run, local slot, and timestamp. Keep invitations and group keys out of diagnostics. A shared field log can contain precise locations even after its keys have been redacted.

The [phone API](../../../docs/PROTOCOL.md) defines replies and status fields. Follow the relevant one through [the Android service](../../../android/app/src/main/java/io/github/miklergm/convoy/ConvoyService.kt) to its destination, such as [the rider's status screen](../../../android/app/src/main/java/io/github/miklergm/convoy/MainActivity.kt) or [the board log](../../../android/app/src/main/java/io/github/miklergm/convoy/BoardLog.kt). In a review, quote a representative safe message when it helps explain the consequence.

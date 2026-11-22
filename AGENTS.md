# Repository instructions

User instructions take precedence over this file and repository skills.

Use the skill that matches the change or review.

- [architecture-review](.agents/skills/architecture-review/SKILL.md) for changes to who owns a fact, what a message means, or how data reaches a board, phone, or map.
- [metrics-value](.agents/skills/metrics-value/SKILL.md) for changes to how delivery, freshness, airtime, or other measurements are defined, collected, or interpreted.
- [actionable-errors](.agents/skills/actionable-errors/SKILL.md) for changes to how a failure, partial result, or recovery is reported to a rider or bench operator.

When a task involves planning, adding, changing, or reviewing automated tests,
read and apply `.agents/skills/test-value/SKILL.md` before making test decisions.
Do not load it merely to run unchanged tests.

A fix is checked where the fault is: a fix to the firmware's C by a test that runs that C, never
only by a change to a model of it, such as the bench's Python board ([tools/bench](tools/bench/README.md)).

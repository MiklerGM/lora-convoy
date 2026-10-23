# Repository instructions

User instructions take precedence over this file and repository skills.

When a task involves planning, adding, changing, or reviewing automated tests,
read and apply `.agents/skills/test-value/SKILL.md` before making test decisions.
Do not load it merely to run unchanged tests.

A fix is checked where the fault is: a fix to the firmware's C by a test that runs that C, never
only by a change to a model of it, such as the bench's Python board ([tools/bench](tools/bench/README.md)).

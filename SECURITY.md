# Security

LoRa Convoy is experimental, and has no releases yet. Its cryptography is our own: AES-128 and
CCM written for the firmware ([ARCHITECTURE](docs/ARCHITECTURE.md), D13), checked against the
FIPS-197, RFC 3610 and OpenSSL vectors and against planted defects, and not yet reviewed by anyone
outside the project. Do not rely on it to keep a group's positions private.

What it is meant to hold ([PROTOCOL](docs/PROTOCOL.md)):

- only a group's members read its positions, and nobody else can make one that is taken;
- a packet played back later is not taken as fresh;
- a board without the key sends nothing.

What it does not hold, by design ([06](docs/06-own-protocol.md), security rules): a member can
send as any rider of the group; whoever holds an invitation, or a lost board, is a member until
the group gets a new key; anyone can see that a group is on the air, and when.

To report a weakness, use GitHub's private vulnerability reporting on this repository, or write
to the address in its commits.

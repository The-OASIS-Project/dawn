# Security Policy

## Reporting a vulnerability

Please **don't open a public issue** for a security problem. Report it privately
through GitHub: the repository's **Security** tab, then **Report a vulnerability**
(<https://github.com/The-OASIS-Project/dawn/security/advisories/new>).

Include what you found, how to reproduce it, which part of DAWN it affects, and
what an attacker could do with it. A proof of concept helps but isn't required.

## What to expect

DAWN is maintained by one developer. You can expect an acknowledgement within
7 days, then an assessment and, for a confirmed issue, a fix on `main` with a
GitHub security advisory. You'll be credited in the advisory unless you'd rather
not be.

## Supported versions

DAWN doesn't publish versioned releases yet; fixes land on `main`. Run a current
`main` and update when an advisory is published.

## Scope

In scope: the daemon, the WebUI, satellites (Raspberry Pi and ESP32), the
`dawn-admin` CLI, and over-the-air updates. [docs/THREAT_MODEL.md](docs/THREAT_MODEL.md)
says what DAWN defends against; a way around any of it is a vulnerability. The
agent acting on instructions injected through content it reads (email, web pages,
documents, messages) counts.

Out of scope, as the threat model describes:

- **Exposing DAWN directly to the internet.** It isn't supported; reach it over a VPN.
- **Physical access** to the host or a satellite.
- **An admin doing what an admin can do.** Admin accounts are trusted by design.
- **Bugs in vendored libraries** ([third_party/README.md](third_party/README.md)):
  report those upstream, and tell us if DAWN's use of the library makes one
  exploitable.

For running DAWN safely, see
[docs/SECURITY_HARDENING_GUIDE.md](docs/SECURITY_HARDENING_GUIDE.md).

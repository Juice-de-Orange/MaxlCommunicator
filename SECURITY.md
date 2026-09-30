# Security Policy

## Supported versions

MaxlCommunicator is developed on `main`; fixes land there. Please test against the latest commit
before reporting.

## Reporting a vulnerability

Please **do not** open a public issue, discussion or pull request for security problems.

Report privately through GitHub's private vulnerability reporting on this repository:
**Security → Report a vulnerability**. Include a description, the commit (and firmware build) you
tested and steps to reproduce.

You will receive an acknowledgement within **7 days**. A fix or workaround is aimed for within
**90 days** of triage, followed by a GitHub security advisory.

## Scope

In scope: the radio link (AES-128-CCM, counters, replay protection, key provisioning and
rotation), the BLE GATT service and its bonding requirements, the PWA and Android bridge, the
dashboard (login, session handling, ingest authentication) and the container setup.

Known and tracked in the issue tracker (no need to report again): the replay window is not yet
persisted across reboots, and the ARQ computes data-frame airtime with a short preamble, so the
duty-cycle budget is underestimated for data frames.

Out of scope: radio regulations in your jurisdiction (see the README's "Regulatory" section).
Physical access to a node is a known limit, not a finding: the network key and device identity are
kept in the MCU's internal flash by design (`CLAUDE.md` §3.0), but the external SPI flash — the
frame counter, the duty-cycle budget and the message queue — is not encrypted and can be read with
a clip.

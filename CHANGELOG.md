# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- First public release: firmware for the LilyGO T-Echo (radio link with AES-128-CCM, persistent
  counters, ARQ, adaptive SF and duty-cycle enforcement; e-paper UI; BLE GATT bridge), the PWA
  bridge, the native Android client (in progress), the dashboard with an append-only event log,
  shared test vectors and the bring-up tooling.
- Dashboard: `DASHBOARD_HOST`, the public host name behind a TLS-terminating reverse proxy.

### Fixed

- Firmware: the duty-cycle budget admitted a data frame as if it carried the 8-symbol preamble
  (226 ms at the default configuration) while the radio sent it with the sniff preamble (2196 ms),
  so the rolling hour could be overrun by up to one frame. The budget question, the retry timer and
  GET_BUDGET's release time now use the frame as it is sent (#1). Host-tested; not yet verified on
  hardware.
- Firmware: the replay window's per-peer high-water mark is persisted, so a frame captured before a
  receiver reboots is rejected after it. Marks are reserved 16 counter values ahead, written once
  per block, and survive a factory reset like the frame counter (#2). Host-tested; not yet verified
  on hardware.

- Dashboard: login and logout answered 403 behind the reverse proxy that `docs/DEPLOYMENT.md`
  prescribes. The cross-site check stays on; set `DASHBOARD_HOST` and rebuild.
- Dashboard: `npm test` emptied the database `DATABASE_URL` points at. The tests now run in their
  own `<name>_test` database and refuse to empty any other.
- Dashboard: `POST /api/ingest` and `POST /api/config` answer 400 instead of 500 for a `nodeId`
  that is not an integer in 0..65535; ingest refuses a request larger than a legitimate batch
  (about 3 MB) with 413 before parsing it; `/nodes/<not a node id>` is a 404.
- Dashboard: `device:add` on an existing node id, `device:rotate` without `--node-id` and
  `db:migrate` without a database print one line instead of a stack trace that included the new
  token's hash.

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

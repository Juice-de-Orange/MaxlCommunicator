# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.1.1] - 2026-10-13

A patch release for the dashboard and the PWA bridge. The firmware, the radio protocol, the BLE
interface and the Android client are unchanged; the firmware version stays 0.1.0.

### Fixed

- Dashboard: sign-in answered 403 "Cross-site POST form submissions are forbidden" whenever the
  dashboard was opened on any `WEB_PORT` other than 4321, and on `http://127.0.0.1:4321`. Without
  `DASHBOARD_HOST`, the login form now accepts `http://localhost:<WEB_PORT>` and
  `http://127.0.0.1:<WEB_PORT>`. `WEB_PORT` is built into the server like `DASHBOARD_HOST`: after
  changing it, `docker compose up -d --build`; a container started with another `WEB_PORT` than
  its image was built for says so and exits (#49).
- Dashboard: the pages link the PWA's icon as their favicon instead of leaving the browser to ask
  for a `/favicon.ico` that does not exist (#49).

### Changed

- Dependencies: vite 8 for the PWA bridge (#33), `@astrojs/react` 7 (#35), `@types/node` 26 in
  `bridge/` and `web/` (#34, #36), and the minor and patch group — astro 7.3.8,
  `@astrojs/node` 11.1.7, drizzle-orm 0.45.4, jsdom 30.1.2 (#44).
- CI runs the production builds of the PWA and the dashboard (#47) and builds the dashboard image
  on every pull request (#48), so a bundler major can no longer pass CI and break the image.

## [0.1.0] - 2026-10-12

The first public release, and the first under the name trailtalk: the project was called
MaxlCommunicator until this release. Everything that runs without hardware is tested on every
build. The two-node radio gates and the power measurements are not yet passed on real devices, and
the firmware fixes below are host-tested only (README, "Status").

### Added

- First public release: firmware for the LilyGO T-Echo (radio link with AES-128-CCM, persistent
  counters, ARQ, adaptive SF and duty-cycle enforcement; e-paper UI; BLE GATT bridge), the PWA
  bridge, the native Android client (in progress), the dashboard with an append-only event log,
  shared test vectors and the bring-up tooling.
- Dashboard: `DASHBOARD_HOST`, the public host name behind a TLS-terminating reverse proxy.

### Changed

- **Renamed from MaxlCommunicator to trailtalk.** The name changes in the README and the docs, the
  dashboard and PWA titles, the PWA manifest and the Android app label; in the packages
  (`trailtalk-dashboard`, `trailtalk-bridge`, `trailtalk-android`), the Kotlin package and the
  Android application id `io.github.juice_de_orange.trailtalk` (no APK was ever published under the
  old one); in the firmware's USB product string, its BLE device name `trailtalk-XXXX` and its image
  files `trailtalk-<env>-<version>-g<hash>`; and in the build variables and C macros, now
  `TRAILTALK_*`. The old `MAXL_*` build variables still work and print a warning; the new name wins
  when both are set, and a release build refuses a development key under either name.
- Unchanged on purpose, so that nodes, phones and installations from before the rename keep
  working: the BLE service and characteristic UUIDs, the radio wire format and its version,
  `BRIDGE_PROTO`, the firmware version, the PWA's storage names and the dashboard's session cookie.
  A dashboard whose `.env` predates the rename keeps running as the compose project `maxl` on its
  volume `maxl_pgdata`; `docs/DEPLOYMENT.md` ("Updating from MaxlCommunicator") has the details.

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
- Firmware: a band, power or sniff-interval change over BLE was charged to the budget at once but
  reached the radio only at the next spreading-factor change, so a node switched from g1 back to g3
  could spend g3's 10 % on 868.1 MHz. The radio is now retuned as soon as it is free, and every
  frame is charged for the band and preamble it was actually sent with (#39). Host-tested; not yet
  verified on hardware.
- Firmware: `FACTORY_RESET` erased the duty-cycle budget ring, so a bonded phone could clear the
  rolling hour and the frame lockout with a reset and a reboot. The ring now survives a reset like
  the frame counter and the replay marks (#40). Host-tested; not yet verified on hardware.
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

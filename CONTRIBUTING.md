# Contributing to MaxlCommunicator

Thanks for taking the time to contribute! Bug reports, measurements from your own boards, ideas and
pull requests are welcome. Larger changes are best discussed in an issue first.

## Before you start

- **`CLAUDE.md` is the normative specification.** Changes to the radio protocol, the bridge
  protocol or the duty-cycle rules start there (or in `docs/bridge-protocol.md`), and the code
  follows. Every wire-format change bumps the version nibble and is recorded in `docs/protocol.md`.
- **Anything that transmits goes through the budget tracker.** There is no second path to the radio.
- **Never power a node without an antenna attached**, flashing included.
- Hardware facts come from `docs/hardware/`. Where the pin map records a contradiction, measure
  rather than guess.

## Development setup

Everything that can run without hardware runs on a normal machine (see the README's
"Quick start without hardware"):

```bash
firmware/tools/hosttest.sh                   # firmware unit tests + simulations (Docker)
python3 firmware/scripts/check_layering.py
cd bridge && npm ci && npm run typecheck && npm test
cd web && cp .env.example .env && npm ci && npm run db:up && npm run db:migrate && npm test
android/tools/test.sh
```

The dashboard tests need a PostgreSQL 17 (`npm run db:up`) but not its data: they create and
migrate their own database, named after the one in `DATABASE_URL` with `_test` appended, and
refuse to empty anything else. `npm run db:migrate` is for the development database itself.

Firmware builds need PlatformIO (`pip install "platformio==6.1.19"`); `pio run -e debug` and
`pio run -e release` in `firmware/`.

### Secret guard

The repository ships a [pre-commit](https://pre-commit.com/) hook that runs
[gitleaks](https://github.com/gitleaks/gitleaks) on every commit:

```bash
pip install pre-commit
pre-commit install
```

CI runs the same scanner over the full history. Never commit a network key, an ingest token, a
password, a serial capture from `docs/test-results/raw/`, your boards' serial numbers or real
coordinates. The development key comes only from the `MAXL_DEV_KEY` environment variable.

## Branch and commit conventions

- Fork, then branch from `main`: `<kind>/<short-slug>` (e.g. `fix/ack-window`, `feat/map-tiles`).
- Commits follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/):
  `feat:`, `fix:`, `docs:`, `test:`, `refactor:`, `ci:`, `chore:`.
- Sign off your commits with the [Developer Certificate of Origin](https://developercertificate.org/):
  `git commit -s`. There is no CLA.

## Pull requests

- A bug fix comes with a test that fails without it — the host test environment covers `link/`,
  `ble/`, `app/`, `ui/` and the portable parts of `hal/`.
- **Radio behaviour changes need a bench test with two devices** before they count as done. Say in
  the PR what ran on hardware and attach the numbers (the format of the reports in
  `docs/test-results/` is a good template). "Green in simulation" is never claimed as a passed gate.
- Keep the layering rules (`firmware/scripts/check_layering.py`) and the no-allocation rule for
  `link/` and `app/`.
- User-facing changes update the README or the component README.

## License

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
Firmware images are distributed under GPL-3.0 because they link GxEPD2 (see `THIRD_PARTY.md`).

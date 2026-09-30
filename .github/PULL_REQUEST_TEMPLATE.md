## What and why

<!-- What does this change and which problem does it solve? Link the issue: Closes #123 -->

## How it was tested

<!-- Commands you ran and what they showed. A bug fix comes with a test that fails without it.
     Radio/firmware changes: what ran on real hardware, with how many devices, and the numbers. -->

## Checklist

- [ ] Commits follow Conventional Commits and are signed off (`git commit -s`)
- [ ] Host tests, bridge, dashboard and Android tests pass where touched
- [ ] Wire-format change? Version nibble bumped and `docs/protocol.md` updated
- [ ] Anything that transmits still goes through the budget tracker
- [ ] No keys, tokens, board serials, serial captures or real coordinates in code, tests or docs

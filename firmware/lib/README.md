# Private libraries

Code that is built separately from `src/` and is therefore **not** subject to the project's
`-Wall -Wextra -Werror` (`platformio.ini` `build_src_flags`). That is the point: vendored
third-party sources belong here, our own code does not.

| Library | What | Licence |
|---|---|---|
| `tinycrypt/` | AES-128-CCM for the radio link (decision D4) | BSD-3-Clause |

Nothing else. Anything we write goes in `src/` under one of the five layers from
`CLAUDE.md` §3, where `scripts/check_layering.py` can see it.

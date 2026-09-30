# Vendored test dependencies

Host-side only. Nothing here is compiled into a firmware image.

| File | Source | Licence |
|---|---|---|
| `doctest.h` | [doctest/doctest](https://github.com/doctest/doctest) v2.4.11, single header | MIT |

doctest rather than Unity (which PlatformIO ships): the failure output names the
actual and expected values, which matters for the airtime and CCM vector tests
where the number *is* the finding. It is one header with no build system of its
own, so the container needs nothing but a compiler.

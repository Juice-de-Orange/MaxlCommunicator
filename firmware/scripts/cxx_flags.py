"""Apply C++-only compiler flags.

CLAUDE.md 6 requires C++17, and the Adafruit core still defaults to gnu++11.
These belong in CXXFLAGS rather than in platformio.ini's build_flags: the latter
are handed to the C compiler too, which then warns on every core source file
that -std=gnu++17, -fno-rtti and -fno-threadsafe-statics are not valid for C.
"""

Import("env")  # noqa: F821

env.Append(  # noqa: F821
    CXXFLAGS=[
        "-std=gnu++17",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-threadsafe-statics",
    ]
)

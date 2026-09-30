"""Give a bench image its node id, so two boards are not both 0x0001.

Every node needs an address on the air, and until 2026-08-31 there was exactly
one: `kDefaultNodeId = 1` in src/main.cpp, and a hardcoded 0x0001 in the
transmitting bring-up sketches. That was correct while there was one board
sending into an empty room. With two boards it is the one configuration that
cannot work -- a frame addressed to 0x0002 that arrives at another 0x0001 is
discarded by its own destination check, and neither side reports anything.

    MAXL_NODE_ID=2 MAXL_BRINGUP=19 pio run -e bringup -t upload --upload-port ...

Unset means 1, which keeps every existing single-node image behaving as before.

This is a BENCH facility, like MAXL_DEV_KEY beside it. The shipping way to set a
node id is over BLE (docs/bridge-protocol.md), and nothing here weakens that: the
define only supplies the value `Node::begin()` starts from.
"""

import os
import sys

Import("env")  # noqa: F821  (injected by PlatformIO)

NODE_ID_VAR = "MAXL_NODE_ID"

raw = os.environ.get(NODE_ID_VAR, "").strip()
if not raw:
    node_id = 1
else:
    try:
        node_id = int(raw, 0)
    except ValueError:
        sys.stderr.write(
            "{} must be a number, got {!r}\n".format(NODE_ID_VAR, raw))
        env.Exit(1)  # noqa: F821
        raise SystemExit(1)

    # 0 is nothing's address and 0xFFFF is broadcast (CLAUDE.md 2.1). A node
    # answering to either would be a node no frame can be addressed to.
    if not 1 <= node_id <= 0xFFFE:
        sys.stderr.write(
            "{} must be between 1 and 0xFFFE; 0 is unassigned and 0xFFFF is "
            "broadcast (CLAUDE.md 2.1). Got {}\n".format(NODE_ID_VAR, node_id))
        env.Exit(1)  # noqa: F821
        raise SystemExit(1)

    print("node id: 0x{:04X}".format(node_id))

env.Append(CPPDEFINES=[("MAXL_NODE_ID", node_id)])  # noqa: F821

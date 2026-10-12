"""Read a TRAILTALK_* build variable, or its name from before the rename.

The project was called MaxlCommunicator until v0.1.0, and every bench variable
carried a MAXL_ prefix: MAXL_DEV_KEY, MAXL_NODE_ID, MAXL_BRINGUP and the link
knobs. A shell profile or a bench script written then must keep building the
image it always built, so the old name is still read -- with a warning on every
build, because a variable that silently stops working one day is worse than one
that complains until it is renamed.

The new name wins when both are set. Not imported by PlatformIO on its own: the
pre-scripts put this directory on sys.path first.
"""

import os

NEW_PREFIX = "TRAILTALK_"
OLD_PREFIX = "MAXL_"


def old_name(name):
    """`TRAILTALK_DEV_KEY` -> `MAXL_DEV_KEY`."""
    return OLD_PREFIX + name[len(NEW_PREFIX):]


def getenv(name, default=""):
    """The stripped value of `name`, else of its pre-rename name, else `default`.

    `name` is the new name, TRAILTALK_ prefix included. An empty value counts as
    unset, as it always has in these scripts.
    """
    old = old_name(name)
    value = os.environ.get(name, "").strip()
    old_value = os.environ.get(old, "").strip()
    if old_value:
        if value:
            print("warning: {} is set and ignored -- {} wins. {} is the name from before "
                  "the rename to trailtalk; unset it.".format(old, name, old))
        else:
            print("warning: {} is the name from before the rename to trailtalk. It still "
                  "works; rename it to {}.".format(old, name))
            return old_value
    return value if value else default

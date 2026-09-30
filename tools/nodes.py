#!/usr/bin/env python3
"""Which /dev/ttyACM* is which node.

    tools/nodes.py --list           every attached T-Echo, with letter and mode
    tools/nodes.py --port-of A      just the path, for shell scripts

Two nodes are attached from 2026-08-31 on, and the port numbers are not stable:
node A came up as ttyACM1 and node B as ttyACM0 on the first evening both were
plugged in, which is the reverse of every hardcoded default in this repository.
Whichever device enumerates first takes ttyACM0, and that is decided by the order
somebody pushed two plugs in.

The USB serial number is stable. It comes from the nRF52840 DEVICEID and is the
same string in both modes -- so it survives flashing, while the by-id path does
not: the bootloader calls itself `usb-LilyGo_T-Echo_v1_<serial>-if00` and the
application `usb-LILYGO_TTGO_eink_<serial>-if00`. Matching on the serial inside
the name is what works across a flash.

The serials live in firmware/nodes.ini, which is also what documents which
physical board is which.
"""

import argparse
import configparser
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
NODES_INI = os.path.join(ROOT, "firmware", "nodes.ini")

# How a board is found, and why it is not simpler.
#
# Neither the USB product id nor the product string identifies anything stable:
# the UF2 bootloader says `T-Echo v1`, the LilyGO factory image says `TTGO_eink`
# with product id 4405, and our own image says `MaxlCommunicator` with product id
# 0029 -- the same id as the bootloader. All three are the same physical board.
# The first version of this file keyed on the product string and stopped finding
# node B the moment we flashed it, which is exactly the failure it exists to
# prevent.
#
# The serial number is the only thing that survives a flash. It comes from the
# nRF52840 DEVICEID, and both the bootloader and the application report it
# unchanged. So: walk sysfs for the vendor id, match the serial, and find the tty
# underneath the device rather than guessing at a /dev/serial/by-id name that
# carries the product string in it.
VENDOR = "239a"

USB_DEVICES = "/sys/bus/usb/devices"

# Only for the human-readable column. Anything unrecognised is reported as such
# rather than guessed at -- the distinction that matters is bootloader or not,
# because a sketch cannot be collected from a board sitting in the bootloader.
BOOTLOADER_PRODUCTS = ("T-Echo v1",)


class NodeError(Exception):
    pass


def configured():
    """{'A': '0123ABCD...', 'B': '89EF4567...'} from firmware/nodes.ini."""
    parser = configparser.ConfigParser()
    if not parser.read(NODES_INI):
        raise NodeError("cannot read {} -- copy firmware/nodes.example.ini to it and "
                        "put in your boards' serials".format(NODES_INI))
    out = {}
    for section in parser.sections():
        if not section.startswith("Node"):
            continue
        serial = parser[section].get("usb_storage_serial", "").strip()
        if serial:
            out[section[len("Node"):].upper()] = serial.upper()
    return out


def _read(path):
    try:
        with open(path) as handle:
            return handle.read().strip()
    except OSError:
        return None


def _tty_of(devpath):
    """/dev/ttyACMn for a USB device, through its CDC interface."""
    for tty in glob.glob(os.path.join(devpath, "*", "tty", "tty*")):
        return os.path.join("/dev", os.path.basename(tty))
    return None


def attached():
    """[(letter_or_None, serial, mode, port)] for every T-Echo on the bus.

    Ordered by letter so --list reads the same way twice, with unknown boards --
    a third one, or one whose serial is not in nodes.ini yet -- last.
    """
    known = {serial: letter for letter, serial in configured().items()}
    found = []
    for devpath in glob.glob(os.path.join(USB_DEVICES, "*-*")):
        if (_read(os.path.join(devpath, "idVendor")) or "").lower() != VENDOR:
            continue
        serial = (_read(os.path.join(devpath, "serial")) or "").upper()
        if not serial:
            continue
        product = _read(os.path.join(devpath, "product")) or "?"
        mode = "bootloader" if product in BOOTLOADER_PRODUCTS else product
        port = _tty_of(devpath)
        if port is None:
            # Enumerated but no CDC interface yet; nothing can be done with it.
            continue
        found.append((known.get(serial), serial, mode, port))
    return sorted(found, key=lambda row: (row[0] is None, row[0] or "", row[1]))


def port_for(letter):
    """The device path for node A/B, or raise with a reason a human can act on."""
    letter = letter.upper()
    serials = configured()
    if letter not in serials:
        raise NodeError("node {} is not in firmware/nodes.ini (have: {})".format(
            letter, ", ".join(sorted(serials)) or "none"))
    serial = serials[letter]
    for found_letter, found_serial, _mode, port in attached():
        if found_serial == serial:
            return port
    raise NodeError(
        "node {} ({}) is not on the USB bus.\n"
        "If it went silent after a crash, only the reset button recovers it:\n"
        "double-tap the upper left button, then try again.".format(letter, serial))


def resolve(node, port):
    """The port to use, given --node and --port, refusing to guess.

    The whole point: with two identical boards attached, defaulting to ttyACM0
    flashes whichever one happened to enumerate first. That is how a night run
    ends up on the wrong device, and the report looks perfectly normal.
    """
    if node and port:
        raise NodeError("--node and --port are mutually exclusive")
    if node:
        return port_for(node)
    if port:
        return port
    boards = attached()
    if len(boards) == 1:
        return boards[0][3]
    if not boards:
        raise NodeError("no T-Echo on the USB bus")
    listing = "\n".join("  {}  {}  {:<17} {}".format(
        letter or "?", serial, mode, path) for letter, serial, mode, path in boards)
    raise NodeError(
        "{} T-Echos are attached and no node was named. Refusing to guess:\n"
        "{}\nPass --node A or --node B.".format(len(boards), listing))


def add_arguments(parser):
    """The pair of options every device tool in tools/ should carry."""
    parser.add_argument("--node", help="A or B, resolved through firmware/nodes.ini")
    parser.add_argument("--port", default=None,
                        help="explicit device path; only needed for a board "
                             "that is not in nodes.ini")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--list", action="store_true")
    group.add_argument("--port-of", metavar="NODE",
                       help="print the device path for node A or B and exit")
    args = parser.parse_args()

    try:
        if args.list:
            boards = attached()
            if not boards:
                print("no T-Echo on the USB bus")
                return 1
            print("{:<5} {:<18} {:<17} {}".format("node", "serial", "mode", "port"))
            for letter, serial, mode, path in boards:
                print("{:<5} {:<18} {:<17} {}".format(
                    letter or "?", serial, mode, path))
            return 0
        print(port_for(args.port_of))
        return 0
    except NodeError as error:
        sys.stderr.write("{}\n".format(error))
        return 2


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Gate 0.5 -- drive the PCF8563 through sketch 03 and record what it does.

    tools/rtc_test.py set-and-reset --node B   set the clock, reset, re-read
    tools/rtc_test.py read --node A            read it now, report the offset
    tools/rtc_test.py pwroff --node A          open decision D2: cut VDD_POWR

`set-and-reset` OVERWRITES the clock, and a drift measurement is nothing but a
clock left alone. With two boards attached that makes --node the difference
between a gate and a lost day, so tools/nodes.py refuses to guess.

`set-and-reset` is the first half of gate 0.5: "RTC keeps time across reset".
`read`, run again hours later, is the second half: drift. The reference is this
machine's clock, so it is only as good as that -- which for a 5 s/day gate is
several orders of magnitude better than it needs to be.
"""

import argparse
import os
import re
import subprocess
import sys
import time

import nodes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PYTHON = os.path.join(ROOT, ".venv", "bin", "python")
# Set in main() from --node/--port; there is no sensible default with two
# identical boards on the bus.
PORT = None

RESULT_RE = re.compile(r"^RESULT ([\w.]+) = (.*)$")


def open_link():
    import serial

    return serial.Serial(PORT, 115200, timeout=1)


def drain(link, seconds, echo=True, until=None):
    """Read for up to `seconds` and return the RESULT lines as a dict.

    `until` names a key to stop on, and its arrival time is returned alongside.
    That timestamp is the whole point: reading the clock and then timestamping at
    the end of a fixed window makes the device look slow by half the window,
    which is exactly how the first run of this test produced a spurious -1.6 s.
    """
    values = {}
    stamp = None
    deadline = time.time() + seconds
    while time.time() < deadline:
        raw = link.readline()
        if not raw:
            continue
        arrived = time.time()
        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if echo:
            print("  " + line)
        match = RESULT_RE.match(line)
        if match:
            values[match.group(1)] = match.group(2)
            if until and match.group(1) == until:
                stamp = arrived
                break
    return (values, stamp) if until else values


def wait_for_port(timeout=30, expect_bounce=False):
    """Wait for the CDC port to be usable again.

    After a reset the old port node lingers for a moment, so simply testing that
    the file exists returns instantly and the caller then opens a device that is
    about to go away. When a bounce is expected, wait for it to disappear first.
    """
    deadline = time.time() + timeout
    if expect_bounce:
        while time.time() < deadline and os.path.exists(PORT):
            time.sleep(0.1)
    while time.time() < deadline:
        if os.path.exists(PORT):
            time.sleep(2.0)
            return True
        time.sleep(0.25)
    return False


def flash():
    env = dict(os.environ, MAXL_BRINGUP="3")
    for args in ([], ["-t", "upload", "--upload-port", PORT]):
        done = subprocess.run(
            [PYTHON, "-m", "platformio", "run", "-d", os.path.join(ROOT, "firmware"),
             "-e", "bringup"] + args,
            env=env, capture_output=True, text=True,
        )
        if done.returncode != 0:
            sys.stderr.write(done.stdout[-3000:])
            return False
    return wait_for_port()


def cmd_set_and_reset(args):
    if args.flash and not flash():
        return 2

    with open_link() as link:
        print("boot report:")
        drain(link, 3)

        reference = int(time.time())
        link.write("TIME {}\n".format(reference).encode())
        link.flush()
        set_at = time.time()
        print("\nset to {} at host time {:.3f}:".format(reference, set_at))
        drain(link, 3)

        print("\nresetting the MCU:")
        link.write(b"RESET\n")
        link.flush()

    if not wait_for_port(expect_bounce=True):
        sys.stderr.write("device did not come back after reset\n")
        return 2

    with open_link() as link:
        print("\nboot report after reset:")
        boot = drain(link, 5)
        # The boot report is repeated on a cadence, so the value in it is however
        # old the last repetition happens to be. For the offset the read has to be
        # bracketed by two host timestamps instead.
        print("\nprecise read:")
        before = time.time()
        link.write(b"READ\n")
        link.flush()
        values, arrived = drain(link, 5, until="rtc.now")

    if "rtc.now" not in values or arrived is None:
        sys.stderr.write("no rtc.now in the reply after reset\n")
        return 2

    # The reply is one line; taking the midpoint of "command out" and "line in"
    # bounds the error by the round trip, which is milliseconds.
    read_at = (before + arrived) / 2
    device = int(values["rtc.now"])
    expected = reference + (read_at - set_at)
    offset = device - expected

    print("\n--- gate 0.5, first half: does the clock survive a reset of the MCU ---")
    reason = boot.get("boot.reset_reason", "?")
    print("  after_soft_reset flag : {}  (from RESETREAS bit 2, SREQ)".format(
        boot.get("boot.after_soft_reset")))
    print("  reset reason          : {}".format(reason))
    # Read from the boot report: the precise read stops at rtc.now by design, and
    # the VL flag is printed after it.
    vl = boot.get("rtc.vl_flag")
    print("  VL flag               : {}  (1 would mean the chip distrusts its own time)".format(vl))
    print("  set to                : {}".format(reference))
    print("  read back             : {}".format(device))
    print("  elapsed on the host   : {:.2f} s".format(read_at - set_at))
    print("  offset                : {:+.2f} s".format(offset))
    # The PCF8563 has one-second resolution and the write lands at an arbitrary
    # point inside a second, so +/-1 s is the quantisation and not drift.
    ok = (abs(offset) <= 1.5
          and vl == "0"
          and boot.get("boot.after_soft_reset") == "1")
    print("  verdict               : {}".format("pass" if ok else "FAIL"))
    print("\n  Reference for the drift half of the gate, run `read` later:")
    print("    RTC_REF_UNIX={} RTC_REF_HOST={:.3f}".format(reference, set_at))
    return 0 if ok else 1


def cmd_read(args):
    with open_link() as link:
        before = time.time()
        link.write(b"READ\n")
        link.flush()
        values, arrived = drain(link, 5, until="rtc.now")
    if "rtc.now" not in values or arrived is None:
        sys.stderr.write("no rtc.now in the reply\n")
        return 2
    read_at = (before + arrived) / 2
    device = int(values["rtc.now"])
    print("\n  device : {}".format(device))
    print("  host   : {}".format(int(read_at)))
    print("  offset : {:+.2f} s".format(device - read_at))
    if args.ref_unix and args.ref_host:
        elapsed = read_at - args.ref_host
        drift = device - (args.ref_unix + elapsed)
        per_day = drift / elapsed * 86400 if elapsed > 0 else 0
        print("\n  measured over {:.0f} s ({:.2f} h)".format(elapsed, elapsed / 3600))
        print("  drift          : {:+.2f} s".format(drift))
        print("  extrapolated   : {:+.2f} s/day   (gate 0.5 allows < 5)".format(per_day))
    return 0


def cmd_pwroff(args):
    with open_link() as link:
        link.write(b"PWROFF\n")
        link.flush()
        drain(link, 8)
    return 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("set-and-reset")
    a.add_argument("--no-flash", dest="flash", action="store_false", default=True)
    a.set_defaults(func=cmd_set_and_reset)

    b = sub.add_parser("read")
    b.add_argument("--ref-unix", type=int)
    b.add_argument("--ref-host", type=float)
    b.set_defaults(func=cmd_read)

    c = sub.add_parser("pwroff")
    c.set_defaults(func=cmd_pwroff)

    for parser_ in (a, b, c):
        nodes.add_arguments(parser_)

    args = parser.parse_args()

    global PORT
    try:
        PORT = nodes.resolve(args.node, args.port)
    except nodes.NodeError as error:
        sys.stderr.write("{}\n".format(error))
        return 2
    print("node: {}".format(PORT))

    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

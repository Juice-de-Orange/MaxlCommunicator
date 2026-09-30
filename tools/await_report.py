#!/usr/bin/env python3
"""Follow a bring-up sketch across resets and return its final report.

tools/bringup_run.py opens the serial port once. That is right for a sketch that
sits there talking, and wrong for one that resets: the port disappears, the file
descriptor is left dead, and every later read returns nothing until the timeout.
It cost two runs on 2026-08-31 -- sketch 04 had passed on the device and looked
like a 900-second silence from the host.

This reopens the port whenever it goes away, which is exactly what a sketch that
resets fifty times needs. It watches for a key to appear with a given value and
returns as soon as it does.

    tools/await_report.py 18 --node B --until gate_2_6.pass --timeout 900
    tools/await_report.py 17 --node B --until gate_2_8a.pass --timeout 4500

Exit code 0 when the key appeared with a non-zero value, 1 when it appeared as
zero, 2 when the timeout ran out first. So a failing gate and a run that never
finished are distinguishable, which a bare pass/fail is not.
"""

import argparse
import os
import re
import sys
import time

import nodes

RESULT_RE = re.compile(r"^RESULT ([\w.]+) = (.*)$")
VERDICT_RE = re.compile(r"^VERDICT (\w+)")

# The port node appears a moment before the CDC endpoint will accept a
# connection; opening too early raises a spurious IO error. Same figure
# bringup_run.py uses, for the same reason.
SETTLE_S = 1.5


def follow(port, until, timeout, echo):
    import serial  # pyserial, in the project venv

    deadline = time.time() + timeout
    seen = {}
    reopens = 0
    link = None

    while time.time() < deadline:
        if link is None:
            if not os.path.exists(port):
                # The device is resetting, or has not come back yet. Both are
                # normal here and neither is an error.
                time.sleep(0.5)
                continue
            time.sleep(SETTLE_S)
            try:
                link = serial.Serial(port, 115200, timeout=1)
                reopens += 1
            except Exception:
                link = None
                time.sleep(0.5)
                continue

        try:
            raw = link.readline()
        except Exception:
            # The port went away mid-read: the sketch reset. Drop it and wait
            # for the node to come back.
            try:
                link.close()
            except Exception:
                pass
            link = None
            continue

        if not raw:
            if not os.path.exists(port):
                try:
                    link.close()
                except Exception:
                    pass
                link = None
            continue

        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
        if echo:
            print(line, flush=True)

        match = RESULT_RE.match(line)
        if match:
            seen[match.group(1)] = match.group(2)
            if match.group(1) == until:
                return match.group(2), seen, reopens

        verdict = VERDICT_RE.match(line)
        if verdict and until == "VERDICT":
            return verdict.group(1), seen, reopens

    return None, seen, reopens


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sketch", type=int, help="only used for the log line")
    nodes.add_arguments(parser)
    parser.add_argument("--until", required=True,
                        help="a RESULT key to wait for, or the word VERDICT")
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    try:
        args.port = nodes.resolve(args.node, args.port)
    except nodes.NodeError as error:
        sys.stderr.write("{}\n".format(error))
        return 2

    value, seen, reopens = follow(args.port, args.until, args.timeout, not args.quiet)

    print("\nawait_report: sketch {}, {} port open(s), {} keys seen".format(
        args.sketch, reopens, len(seen)))

    if value is None:
        sys.stderr.write("await_report: '{}' never appeared within {}s\n".format(
            args.until, args.timeout))
        return 2

    print("await_report: {} = {}".format(args.until, value))
    if args.until == "VERDICT":
        return 0 if value in ("pass", "inconclusive") else 1
    return 0 if value.strip() not in ("0", "") else 1


if __name__ == "__main__":
    sys.exit(main())

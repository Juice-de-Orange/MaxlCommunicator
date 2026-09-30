#!/usr/bin/env python3
"""Build, flash and collect one phase 0 bring-up sketch.

    tools/bringup_run.py 2 --node A        build, flash, collect, print
    tools/bringup_run.py 2 --node A --no-flash   collect from a running device
    tools/bringup_run.py 2 --node A --cycles 3   three full report cycles

With two boards attached, --node is not optional: see tools/nodes.py for why
a hardcoded /dev/ttyACM0 flashes whichever one enumerated first.

The sketches repeat their report forever, because anything printed before the
host opens the port is lost. This waits for a `MAXL-BRINGUP nn begin`, collects
through the matching `end`, and stops -- so the output is one complete run
rather than a slice of the middle of one.

Exit code follows the sketch's own VERDICT: 0 for pass, 0 for inconclusive (that
is a fact about the test, not a failure of the board), 1 for fail.
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
FIRMWARE = os.path.join(ROOT, "firmware")
PYTHON = os.path.join(ROOT, ".venv", "bin", "python")

BEGIN_RE = re.compile(r"^MAXL-BRINGUP (\d+) begin\s*$")
END_RE = re.compile(r"^MAXL-BRINGUP (\d+) end\s*$")
VERDICT_RE = re.compile(r"^VERDICT (\w+)")


def pio(args, sketch):
    env = dict(os.environ, MAXL_BRINGUP=str(sketch))
    return subprocess.run(
        [PYTHON, "-m", "platformio", "run", "-d", FIRMWARE, "-e", "bringup"] + args,
        env=env,
        capture_output=True,
        text=True,
    )


def wait_for_port(port, timeout):
    """The device drops off USB while the bootloader hands over to the app."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(port):
            return True
        time.sleep(0.25)
    return False


def collect(port, sketch, cycles, timeout):
    import serial  # pyserial, in the project venv

    deadline = time.time() + timeout
    collected = []
    inside = False
    done = 0

    with serial.Serial(port, 115200, timeout=1) as link:
        while time.time() < deadline and done < cycles:
            raw = link.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            begin = BEGIN_RE.match(line)
            if begin and int(begin.group(1)) == sketch:
                inside = True
                collected = [line] if done == 0 else collected + [line]
                continue
            if inside:
                collected.append(line)
                end = END_RE.match(line)
                if end and int(end.group(1)) == sketch:
                    inside = False
                    done += 1
    return collected, done


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sketch", type=int)
    nodes.add_arguments(parser)
    parser.add_argument("--no-flash", action="store_true")
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=90.0)
    args = parser.parse_args()

    try:
        args.port = nodes.resolve(args.node, args.port)
    except nodes.NodeError as error:
        sys.stderr.write("{}\n".format(error))
        return 2

    if not args.no_flash:
        print("building sketch {} ...".format(args.sketch))
        built = pio([], args.sketch)
        if built.returncode != 0:
            sys.stderr.write(built.stdout[-4000:] + built.stderr[-2000:])
            return 2
        print("flashing ...")
        flashed = pio(["-t", "upload", "--upload-port", args.port], args.sketch)
        if flashed.returncode != 0:
            sys.stderr.write(flashed.stdout[-4000:] + flashed.stderr[-2000:])
            return 2
        if not wait_for_port(args.port, 30):
            sys.stderr.write(
                "{} never came back. The dead man's timer should return the "
                "device to TECHOBOOT shortly; check with lsusb.\n".format(args.port)
            )
            return 2
        # The port node appears a moment before the CDC endpoint will accept a
        # connection; opening too early raises a spurious IO error.
        time.sleep(1.5)

    lines, done = collect(args.port, args.sketch, args.cycles, args.timeout)
    for line in lines:
        print(line)

    if done == 0:
        sys.stderr.write("\nno complete report cycle within {}s\n".format(args.timeout))
        return 2

    verdicts = [VERDICT_RE.match(l).group(1) for l in lines if VERDICT_RE.match(l)]
    print("\nverdicts: {}".format(", ".join(verdicts) or "none"))
    return 1 if "fail" in verdicts else 0


if __name__ == "__main__":
    sys.exit(main())

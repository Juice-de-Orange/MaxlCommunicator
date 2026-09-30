#!/usr/bin/env bash
#
# Everything that can run unattended once the device is reachable again.
#
# Five gates in about ninety minutes, plus a six-hour run that is meant to go
# overnight. None of it needs hands -- but all of it needs a device on
# /dev/ttyACM0, so run it after the hands-on bring-up steps in docs/test-plan.md.
#
# Each step flashes, then follows the port with tools/await_report.py rather than
# tools/bringup_run.py. That is not a preference: sketches 16, 17 and 18 reset
# themselves, and bringup_run.py opens the port exactly once. When the device
# resets, its file descriptor is left dead and every later read returns nothing
# until the timeout -- which on 2026-08-31 made a passing gate look like a 900
# second silence, twice.
#
#   tools/night.sh --node B             everything, in order
#   tools/night.sh --node B 16 18       only those steps
#
# --node is required, not defaulted. Two identical boards are attached from
# 2026-08-31 on and the port numbers depend on plug order alone -- on the first
# evening node A came up as ttyACM1, so the old hardcoded /dev/ttyACM0 would have
# spent ninety unattended minutes on the wrong device. tools/nodes.py resolves
# the letter through the USB serial in firmware/nodes.ini.
#
# Sketch 17 needs a VALID CLOCK: it reads the RTC and never sets it, and a node
# without time is fully transmit-blocked by CLAUDE.md 1.2, so gate 2.7 would
# measure nothing. On a board whose clock has never been set, run
# `tools/rtc_test.py set-and-reset --node <X>` first -- but never on a board
# carrying a drift measurement.
#
# ORDER MATTERS. Two things are deliberately last:
#
#   3.4  occupies the device for six hours
#   2.9  clears the RTC, and with it the drift measurement gate 0.5 depends on.
#        Do NOT run it before the drift has been read.
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
OUT="$ROOT/docs/test-results/raw"
STAMP="$(date +%Y-%m-%d_%H%M)"

mkdir -p "$OUT"

NODE=""
rest=()
while [ $# -gt 0 ]; do
    case "$1" in
        --node) NODE="${2:-}"; shift 2 ;;
        --node=*) NODE="${1#--node=}"; shift ;;
        *) rest+=("$1"); shift ;;
    esac
done
set -- ${rest+"${rest[@]}"}

if [ -z "$NODE" ]; then
    cat >&2 <<'MSG'
tools/night.sh needs --node A or --node B.

Two identical boards are attached and which one owns /dev/ttyACM0 is decided by
plug order. This run flashes and transmits for ninety minutes unattended; it is
not going to guess which board that happens to.

    tools/nodes.py --list
MSG
    exit 1
fi

if ! PORT="$("$PY" "$ROOT/tools/nodes.py" --port-of "$NODE")"; then
    exit 1
fi
echo "node $NODE -> $PORT"

if [ ! -e "$PORT" ]; then
    cat >&2 <<'MSG'
That port is not there.

If the device is silent after a crash, only the reset button recovers it: with
CFG_DEBUG=1 the fault handler halts at a breakpoint rather than resetting, and
PIN_PWR_ON latches the battery so unplugging USB does not reset it either.
Double-tap the upper left button, then run this again.
MSG
    exit 1
fi

# sketch : the RESULT key that says it finished : seconds to allow
# Sketch 16's far side was given 180 s on 2026-08-31 and did not make it: the
# report never came, and the board was in the bootloader afterwards. The second
# run, with 420 s, passed gate 3.1 in well under that. The reading that fits both
# is that the FIRST far side is slow -- it lays out the queue and journal regions
# on a chip that has never held them -- and that it overran both this budget and
# the sketch's own 180 s dead man window, which nothing pokes while that work
# runs. 600 s here covers the first case; the window in the sketch is the other
# half and is not fixed yet.
STEPS=(
    "16:gate_3_1.pass:600"
    "18:gate_2_6.pass:1200"
    "17:gate_2_8a.pass:5400"
)

wanted=("$@")
if [ ${#wanted[@]} -eq 0 ]; then
    wanted=(16 18 17)
fi

echo "=== night run, $STAMP ==="
echo "steps: ${wanted[*]}"
echo

failed=()
for step in "${STEPS[@]}"; do
    sketch="${step%%:*}"
    rest="${step#*:}"
    key="${rest%%:*}"
    timeout="${rest##*:}"

    keep=0
    for w in "${wanted[@]}"; do [ "$w" = "$sketch" ] && keep=1; done
    [ $keep -eq 1 ] || continue

    log="$OUT/${STAMP}_sketch$(printf '%02d' "$sketch").log"
    printf -- "--- sketch %02d, waiting for %s (up to %ss) ---\n" "$sketch" "$key" "$timeout"

    if ! MAXL_BRINGUP="$sketch" "$PY" -m platformio run -d "$ROOT/firmware" -e bringup \
            -t upload --upload-port "$PORT" >"$log" 2>&1; then
        echo "  -> FLASH FAILED  ($log)"
        failed+=("$sketch")
        break
    fi

    if "$PY" "$ROOT/tools/await_report.py" "$sketch" --node "$NODE" \
            --until "$key" --timeout "$timeout" \
            2>&1 | tee -a "$log" | tail -3; then
        echo "  -> pass  ($log)"
    else
        echo "  -> FAILED or timed out  ($log)"
        failed+=("$sketch")
        # Keep going: a later gate does not depend on an earlier one, and
        # stopping would waste the night on the first surprise.
    fi
    echo
done

echo "=== done ==="
if [ ${#failed[@]} -gt 0 ]; then
    echo "failed: ${failed[*]}"
fi

cat <<'MSG'

Still to do by hand, and in this order:

  the drift readout        tools/bringup_run.py 3 --node A
                           tools/rtc_test.py read --node A \
                                                  --ref-unix 1788126105 \
                                                  --ref-host 1788126105.632
  THEN gate 2.9            clearing the RTC destroys that measurement, so it
                           cannot come first

  gate 3.4                 six hours, telemetry interval within 10 %
MSG

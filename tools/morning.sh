#!/usr/bin/env bash
#
# Everything the bring-up can do on its own, once the cable is back in.
#
# The session of 2026-08-31 left the hardware track blocked on one physical act:
# the host's xHCI port gave up after the flash cycle test and only an unplug and
# replug brings it back. This is what to run afterwards.
#
# It is deliberately not clever. Each sketch is flashed, its report collected,
# and the raw output kept -- and it stops at the first hard failure rather than
# carrying on and burying it. docs/test-plan.md wants gates with numbers written
# down; this writes them down.
#
#   tools/morning.sh --node A            run everything that needs no hands
#   tools/morning.sh --node A 4 10       run only those sketches
#
# --node is required: two identical boards are attached from 2026-08-31 on, and
# which of them owns /dev/ttyACM0 depends on nothing but plug order. See
# tools/nodes.py.
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
    echo "tools/morning.sh needs --node A or --node B. See tools/nodes.py --list." >&2
    exit 1
fi

if ! PORT="$("$PY" "$ROOT/tools/nodes.py" --port-of "$NODE")"; then
    exit 1
fi

# Sketches that report and stop. 08 leaves a test pattern on the panel for a
# human to judge; 09 is left running for somebody to press the buttons.
DEFAULT_SKETCHES=(4 10 2 3 5 6 7 8)

sketches=("$@")
if [ ${#sketches[@]} -eq 0 ]; then
    sketches=("${DEFAULT_SKETCHES[@]}")
fi

if [ ! -e "$PORT" ]; then
    cat >&2 <<'MSG'
That port is not there.

If the device is in the bootloader, TECHOBOOT should be mounted and the port
should exist. If neither is true, the host's USB port is still wedged -- unplug
the cable, wait a moment, plug it back in. That is the whole fix; nothing on the
board is broken.
MSG
    exit 1
fi

echo "=== bring-up, $STAMP ==="
echo "node: $NODE -> $PORT"
echo "sketches: ${sketches[*]}"
echo

failed=()
for n in "${sketches[@]}"; do
    log="$OUT/${STAMP}_sketch$(printf '%02d' "$n").log"
    printf -- "--- sketch %02d ---\n" "$n"

    # Sketch 04 runs a hundred reboot cycles and takes several minutes; the
    # others answer in seconds.
    timeout=180
    [ "$n" = "4" ] && timeout=900

    if "$PY" "$ROOT/tools/bringup_run.py" "$n" --node "$NODE" \
            --timeout "$timeout" 2>&1 | tee "$log"; then
        verdict=$(grep -oE 'VERDICT (pass|fail|inconclusive)' "$log" | tail -1)
        echo "  -> ${verdict:-no verdict}  ($log)"
    else
        echo "  -> FAILED  ($log)"
        failed+=("$n")
        # A sketch that cannot even be flashed usually means the port went away
        # again, and every later sketch would fail the same way for the same
        # reason. Stopping keeps the log readable.
        if ! [ -e "$PORT" ]; then
            echo
            echo "The port disappeared. Stopping -- the rest would fail identically."
            break
        fi
    fi
    echo
done

echo "=== done ==="
if [ ${#failed[@]} -gt 0 ]; then
    echo "failed: ${failed[*]}"
fi

cat <<'MSG'

Still needs a person:

  RTC drift          tools/rtc_test.py read --node A --ref-unix 1788126105 \
                                            --ref-host 1788126105.632
                     (the clock must NOT have been re-set in between)
  Gate 0.2 / 1.2     look at the panel. Sketch 08 left a test pattern on it and
                     it survives with no power: a one-pixel border, filled
                     squares in three corners only, both diagonals crossing at
                     the centre, and a ruler of one-pixel lines. A mirrored or
                     offset frame is obvious at a glance.
  Gate 0.4           flash deep-power-down current, with a meter
  Gate 1.4 / 1.5     500 button presses and 200 touches. Sketch 09 counts them:
                     flash it, press, then read the report.
  Gate 1.6           battery ADC against a multimeter at three points

And then hal/board.h, with each pin carrying the sketch number and date that
confirmed it.
MSG

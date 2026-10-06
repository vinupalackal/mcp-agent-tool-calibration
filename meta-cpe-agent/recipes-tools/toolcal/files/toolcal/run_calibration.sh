#!/bin/sh
# run_calibration.sh - calibrate every tool in a catalog under idle, typical
# and stressed load, then build the threshold report.
#
# usage: run_calibration.sh -c tools.json [options]
#   -c FILE   tool catalog (required)
#   -o DIR    output directory            (default ./calib-<timestamp>)
#   -n N      runs per tool, overrides the catalog setting
#   -C LIST   conditions to run            (default "idle typical stressed")
#   -T CMD    command that creates typical load, e.g. "iperf3 -c 192.168.0.10 -t 3600"
#             (or set TYPICAL_LOAD_CMD); without it the typical condition is skipped
#   -m PCT    memory held by the stress load, % of MemTotal   (default 50)
#   -s SEC    settle time after starting a load               (default 10)
#   -b BASIS  budget basis: max (worst condition) or a condition name (default max)
#
# Works with BusyBox sh. Uses stress-ng for the stressed condition when it is
# installed, otherwise the built-in "toolcal hog". Run as root on the target.

set -u

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

CFG=""
OUT="./calib-$(date +%Y%m%d-%H%M%S)"
RUNS=""
CONDS="idle typical stressed"
TYPICAL_CMD="${TYPICAL_LOAD_CMD:-}"
MEM_PCT=50
SETTLE=10
BASIS="max"

while getopts "c:o:n:C:T:m:s:b:h" opt; do
    case "$opt" in
        c) CFG=$OPTARG ;;
        o) OUT=$OPTARG ;;
        n) RUNS=$OPTARG ;;
        C) CONDS=$OPTARG ;;
        T) TYPICAL_CMD=$OPTARG ;;
        m) MEM_PCT=$OPTARG ;;
        s) SETTLE=$OPTARG ;;
        b) BASIS=$OPTARG ;;
        *) usage ;;
    esac
done
[ -n "$CFG" ] || usage
[ -r "$CFG" ] || { echo "cannot read catalog: $CFG" >&2; exit 2; }

TOOLCAL="${TOOLCAL:-}"
if [ -z "$TOOLCAL" ]; then
    if [ -x "$(dirname "$0")/toolcal" ]; then TOOLCAL="$(dirname "$0")/toolcal"
    else TOOLCAL=$(command -v toolcal || true); fi
fi
[ -n "$TOOLCAL" ] && [ -x "$TOOLCAL" ] || { echo "toolcal binary not found (set TOOLCAL=/path/to/toolcal)" >&2; exit 2; }
[ "$(id -u)" = "0" ] || echo "warning: not root; measurements fall back to rusage (no cgroups)" >&2

NCPU=$(grep -c '^processor' /proc/cpuinfo 2>/dev/null || echo 1)
[ "$NCPU" -ge 1 ] 2>/dev/null || NCPU=1
MEMTOTAL_KB=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
HOG_MB=$(( MEMTOTAL_KB * MEM_PCT / 100 / 1024 ))

mkdir -p "$OUT"
{
    echo "date:     $(date)"
    echo "kernel:   $(uname -a)"
    echo "cpus:     $NCPU"
    echo "memtotal: ${MEMTOTAL_KB} kB"
    grep -m1 -E 'model name|Hardware|cpu model' /proc/cpuinfo 2>/dev/null
    [ -r /version.txt ] && head -n 3 /version.txt
    echo "psi:      $( [ -r /proc/pressure/cpu ] && echo yes || echo no )"
    echo "cgroup2:  $( grep -q cgroup2 /proc/mounts && echo yes || echo no )"
} > "$OUT/device.txt"
"$TOOLCAL" list -c "$CFG" > "$OUT/catalog.txt" || exit 2
cat "$OUT/catalog.txt"

# Start a load in its own session when setsid exists (BusyBox may omit it),
# so the whole load can be stopped by process group.
if command -v setsid > /dev/null 2>&1; then BG="setsid"; else BG=""; fi

LOAD_PID=""
start_load() {
    LOAD_PID=""
    case "$1" in
        idle) return 0 ;;
        typical)
            if [ -z "$TYPICAL_CMD" ]; then
                echo "skipping 'typical': no load command given (-T or TYPICAL_LOAD_CMD)" >&2
                return 1
            fi
            $BG sh -c "$TYPICAL_CMD" > "$OUT/typical_load.log" 2>&1 &
            LOAD_PID=$!
            ;;
        stressed)
            if command -v stress-ng > /dev/null 2>&1; then
                $BG stress-ng --cpu "$NCPU" --vm 1 --vm-bytes "${MEM_PCT}%" --vm-keep > "$OUT/stress.log" 2>&1 &
            else
                $BG "$TOOLCAL" hog --cpu "$NCPU" --mem-mb "$HOG_MB" > "$OUT/stress.log" 2>&1 &
            fi
            LOAD_PID=$!
            ;;
        *)
            echo "unknown condition '$1' (use idle, typical, stressed)" >&2
            return 1
            ;;
    esac
    return 0
}

stop_load() {
    [ -n "$LOAD_PID" ] || return 0
    if [ -n "$BG" ]; then
        kill -TERM -- "-$LOAD_PID" 2>/dev/null || kill -TERM "$LOAD_PID" 2>/dev/null
        sleep 1
        kill -KILL -- "-$LOAD_PID" 2>/dev/null
    else
        pkill -TERM -P "$LOAD_PID" 2>/dev/null
        kill -TERM "$LOAD_PID" 2>/dev/null
        sleep 1
        pkill -KILL -P "$LOAD_PID" 2>/dev/null
        kill -KILL "$LOAD_PID" 2>/dev/null
    fi
    wait "$LOAD_PID" 2>/dev/null
    LOAD_PID=""
}

trap 'stop_load; echo "interrupted" >&2; exit 130' INT TERM

CSVS=""
for cond in $CONDS; do
    echo
    echo "=== condition: $cond ==="
    rm -f "$OUT/$cond/runs.csv"
    start_load "$cond" || continue
    [ "$cond" = "idle" ] || { echo "settling for ${SETTLE}s"; sleep "$SETTLE"; }
    if [ -n "$RUNS" ]; then
        "$TOOLCAL" run -c "$CFG" -o "$OUT/$cond" -l "$cond" -n "$RUNS"
    else
        "$TOOLCAL" run -c "$CFG" -o "$OUT/$cond" -l "$cond"
    fi
    rc=$?
    stop_load
    [ $rc -eq 0 ] || { echo "run interrupted in '$cond'" >&2; exit $rc; }
    CSVS="$CSVS $OUT/$cond/runs.csv"
done

[ -n "$CSVS" ] || { echo "no condition was measured" >&2; exit 2; }
echo
echo "=== threshold report ==="
# shellcheck disable=SC2086
"$TOOLCAL" report -c "$CFG" -o "$OUT/threshold_report" -b "$BASIS" $CSVS
rc=$?
echo
echo "all results in: $OUT"
exit $rc

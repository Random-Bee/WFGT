#!/usr/bin/env bash
set -euo pipefail

# The runs that answer the review, one after another: the work and the time of
# a direction rule that counts each vertex of a frontier once
# (work/wf-dir-count-exact.cpp and wf-dir-exact.cpp), those of uniformly random
# orders against affine ones, and those of whole lists against chunks of 256
# neighbors on the graph of hubs.
# Each step takes the benchmark lock itself; a step that fails is reported, and
# the next one runs. STEPS="exact-counts exact-times" runs only those.
#   setsid nohup ./review-runs.sh > review-runs.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/.." && pwd)
bench="$root/benchmark"
order_builds="b256 uniform"
whole_builds="whole b256"
graphs="lj twitter road uk kron hubs"

step() {
    local name=$1 log=$2 status=0
    shift 2
    echo "$(date '+%F %T') start $name"
    "$@" > "$log" 2>&1 || status=$?
    if [ "$status" = 0 ]; then
        echo "$(date '+%F %T') done $name"
    else
        echo "$(date '+%F %T') FAILED $name (exit $status), see $log"
    fi
}

read -r -a steps <<< "${STEPS:-exact-counts exact-times orders-counts orders-times whole-counts whole-times}"
for name in "${steps[@]}"; do
    case $name in
        exact-counts) step exact-counts "$here/work/work-counts-exact.log" \
                          env OUT=work-counts-exact WD_NAME=wdexact WD_VARIANT=exact BASELINES=0 \
                          STALL_GRAPHS= GRAPHS="$graphs" bash "$here/work/work-counts.sh" ;;
        exact-times) step exact-times "$bench/results/exact.log" \
                         env RESULTS=exact PROGRAMS="wd wdexact" COUNTS="16 64 96 112" \
                         bash "$bench/benchmark.sh" both "$graphs" ;;
        orders-counts) step orders-counts "$here/chunks/orders-counts.log" \
                           env OUT=orders-counts BUILDS="$order_builds" GRAPHS="$graphs" \
                           bash "$here/chunks/chunk-counts.sh" ;;
        orders-times) step orders-times "$here/chunks/orders-times.log" \
                          env OUT=orders-times BUILDS="$order_builds" GRAPHS="$graphs" \
                          bash "$here/chunks/chunk-times.sh" ;;
        whole-counts) step whole-counts "$here/chunks/whole-counts.log" \
                          env OUT=whole-counts BUILDS="$whole_builds" GRAPHS=hubs \
                          bash "$here/chunks/chunk-counts.sh" ;;
        whole-times) step whole-times "$here/chunks/whole-times.log" \
                         env OUT=whole-times BUILDS="$whole_builds" GRAPHS=hubs \
                         bash "$here/chunks/chunk-times.sh" ;;
        *) echo "unknown step $name" >&2; exit 1 ;;
    esac
done
echo "$(date '+%F %T') all done"

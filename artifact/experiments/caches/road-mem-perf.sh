#!/usr/bin/env bash
set -euo pipefail

# Why WF-DO-mem is faster than WF-DO on road-USA, and how the two compare on
# the graph GRAPH names (lj, twitter, uk, kron or hubs): user-mode counts of stores,
# read-for-ownership requests and load misses over the 20 timed searches of the
# benchmark's builds of wd and mem for both sockets (benchmark/build, shift 0),
# at THREADS (1 16 40 112 by default), in two passes so that no counter is
# multiplexed. Rows go to road-mem-perf.csv, or <graph>-mem-perf.csv for
# another graph, as program,threads,event,count:
#   setsid nohup ./road-mem-perf.sh > road-mem-perf.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
graph_name=${GRAPH:-road}
set_graph "$graph_name" || { echo "unknown graph $graph_name" >&2; exit 1; }
graph=$path
out="$here/$graph_name-mem-perf.csv"
read -r -a counts <<< "${THREADS:-1 16 40 112}"
passes=("cycles:u,instructions:u,mem_inst_retired.all_stores:u,l2_rqsts.all_rfo:u,l2_rqsts.rfo_miss:u,offcore_requests.demand_rfo:u"
        "mem_load_retired.l2_miss:u,mem_load_retired.l3_miss:u,longest_lat_cache.miss:u,dtlb_load_misses.walk_completed:u,dtlb_store_misses.walk_completed:u")

for file in "$graph" "$ref" "$root/benchmark/build/wd-0.out" "$root/benchmark/build/mem-0.out"; do
    [ -e "$file" ] || { echo "missing $file" >&2; exit 1; }
done
command -v "$perf" > /dev/null || { echo "missing $perf (BFS_PERF in config.sh)" >&2; exit 1; }

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T') on $(hostname)"

work=$(mktemp -d "$here/road-mem-perf.XXXXXX")
control_fifo="$work/control"
ack_fifo="$work/ack"
mkfifo "$control_fifo" "$ack_fifo"
cleanup() {
    rm -f -- "$control_fifo" "$ack_fifo" "$work/wfd-out.txt" "$work/stat.txt" "$work/stdout.txt"
    rmdir -- "$work" 2>/dev/null || true
}
trap cleanup EXIT
cd "$work"
export PERF_CTL_FIFO="$control_fifo" PERF_ACK_FIFO="$ack_fifo"

echo "program,threads,event,count" > "$out"
for threads in "${counts[@]}"; do
    for program in wd mem; do
        binary="$root/benchmark/build/$program-0.out"
        for events in "${passes[@]}"; do
            rm -f -- wfd-out.txt stat.txt
            "$perf" stat -D -1 --control "fifo:$control_fifo,$ack_fifo" -x, -o stat.txt -e "$events" -- \
                env BFS_SPREAD_NODES=1 numactl --interleave=all "$binary" "$graph" "$threads" \
                > stdout.txt 2>&1
            if ! cmp -s wfd-out.txt "$ref"; then
                echo "$program at $threads threads: wrong distances" >&2
                exit 1
            fi
            awk -F, -v program="$program" -v threads="$threads" \
                '$1 ~ /^[0-9]+$/ { sub(/:u$/, "", $3); printf "%s,%s,%s,%s\n", program, threads, $3, $1 }' \
                stat.txt >> "$out"
            echo "$(date +%T) $program $threads: $(awk 'NF == 1 && $1 ~ /^[0-9]+$/ { print $1 / 1000 " ms" }' stdout.txt | head -1)"
        done
    done
done
echo "done $(date '+%F %T')"

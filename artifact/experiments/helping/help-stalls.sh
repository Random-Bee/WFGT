#!/usr/bin/env bash
set -euo pipefail

# The work that helpers do in WF-DO in the experiment of ../stalls/stalls.sh
# that varies the length of a stall: LiveJournal, or the graph GRAPH names
# (twitter, road, uk, kron or hubs), from vertex 0 on 112 threads spread over
# both NUMA nodes, with memory interleaved, and the last thread busy-waiting 0
# to 10000 us (0 to 1000 us on Road-USA, as in stalls.sh) at the start of every
# level. One program, built as stalls.sh builds its own:
#   count        ../work/wf-dir-count-stalls.cpp, which counts the work that owners and
#                helpers do; counting slows it, so only its counts matter
# Every setting runs RUNS times (5 by default), a run being 20 searches. A run
# is correct when its distances match a sequential BFS's. Rows go to
# help-stalls-both.csv next to this script (help-stalls-<graph>.csv for another
# graph), and per setting the medians of the correct runs to the matching
# -summary.csv. A stopped run picks up where it left off:
#   setsid nohup ./help-stalls.sh > help-stalls.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-5}
threads=112

graph_name=${GRAPH:-lj}
case $graph_name in
    lj) suffix=both ;;
    twitter|road|uk|kron|hubs) suffix=$graph_name ;;
    *) echo "unknown graph $graph_name" >&2; exit 1 ;;
esac
set_graph "$graph_name"
graph=$path
build="$here/help-build"
out="$here/help-stalls-$suffix.csv"
summary="$here/help-stalls-$suffix-summary.csv"
programs=(count)
lengths=(0 10 20 50 100 200 500 1000 2000 5000 10000)
[ "$graph_name" != road ] || lengths=(0 10 20 50 100 200 500 1000)
# the fields of a "help" line of count, in the order of the columns
count_keys="v00 v01 v10 v11 e00 e01 e10 e11 b00 b01 b10 b11 repeated bu_levels blocks repeated_edges"

for file in "$graph" "$ref"; do
    [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
done

declare -A done_run
if [ -f "$out" ]; then
    while IFS=, read -r us run name rest; do
        correct=$(echo "$rest" | cut -d, -f3)
        if [ "$correct" = yes ]; then
            done_run["$us,$run,$name"]=1
        fi
    done < <(tail -n +2 "$out")
fi

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T') on $(hostname), $("$cxx" --version | awk 'NR == 1')"

# GCC's intermediate files go here rather than in /tmp
export TMPDIR="$build/tmp"
mkdir -p "$TMPDIR" "$build/run"
flags=(-std=c++23 -O3 -march=native -DNDEBUG -g -fno-omit-frame-pointer
       -falign-loops=32 -falign-jumps=32 -include "$root/benchmark/text-shift.h"
       -DTEXT_SHIFT=0 -pthread)
"$cxx" "${flags[@]}" "$here/../work/wf-dir-count-stalls.cpp" -o "$build/count.out"
echo "built ${#programs[@]} programs"

if [ ! -f "$out" ]; then
    echo "stall_us,run,program,us,last_us,correct,${count_keys// /,},finished" > "$out"
fi
cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

for run in $(seq 1 "$runs"); do
    for us in "${lengths[@]}"; do
        for i in "${!programs[@]}"; do
            name=${programs[(i + run) % ${#programs[@]}]}
            key="$us,$run,$name"
            if [ -n "${done_run[$key]:-}" ]; then
                continue
            fi
            rm -f -- wfd-out.txt
            status=0
            env BFS_SPREAD_NODES=1 numactl --interleave=all env BFS_STALLED=1 BFS_STALL_US="$us" \
                "$build/$name.out" "$graph" "$threads" < /dev/null > stdout.txt 2> stderr.txt ||
                status=$?
            # for one thread count a program prints just its time
            time=$(awk 'NF == 1 && $1 ~ /^[0-9]+$/ { t = $1 } END { print t }' stdout.txt)
            last=$(awk '$1 == "last" && $2 == "return" && $3 ~ /^[0-9]+$/ { t = $3 } END { print t }' \
                stderr.txt)
            fields=$(awk -v keys="$count_keys" '
                $1 == "help" { for (i = 2; i < NF; i += 2) c[$i] = $(i + 1) }
                END { n = split(keys, k, " ")
                      for (i = 1; i <= n; i++) printf "%s%s", (i > 1 ? "," : ""), c[k[i]] }' \
                stderr.txt)
            correct=no
            if [ "$status" = 0 ] && [ -n "$time" ] && cmp -s wfd-out.txt "$ref"; then
                correct=yes
            fi
            echo "$key,${time:-failed},$last,$correct,$fields,$(date +%FT%T)" >> "$out"
            echo "$(date +%T) $key: ${time:-failed} us${last:+, last return $last us}, correct $correct"
            if [ "$correct" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done
done

# per setting and program, the median of each column over the correct runs
{
    echo "stall_us,program,runs,us,last_us,${count_keys// /,}"
    # the last column is when the run finished
    awk -F, 'NR > 1 && $6 == "yes" {
                 key = $1 "," $3; runs[key]++; last = NF - 1
                 for (c = 4; c <= last; c++) if (c != 6 && $c != "") v[key, c] = v[key, c] " " $c }
             function median(list,    a, k) {
                 k = split(list, a, " "); if (!k) return ""; asort(a)
                 return (k % 2) ? a[(k + 1) / 2] : (a[k / 2] + a[k / 2 + 1]) / 2 }
             END { for (key in runs) {
                       line = key "," runs[key]
                       for (c = 4; c <= last; c++) if (c != 6) line = line "," median(v[key, c])
                       print line } }' "$out" | sort -t, -k1,1n -k2,2
} > "$summary"
cat -- "$summary"
echo "finished $(date '+%F %T')"

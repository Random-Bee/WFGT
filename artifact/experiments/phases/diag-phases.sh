#!/usr/bin/env bash
set -euo pipefail

# a search of our methods split into its two phases, on the graphs of GRAPHS
# (LiveJournal, Twitter, Road-USA, uk-2002, Kronecker and the graph of hubs by
# default) from vertex 0 over both sockets, at 112 and then 128
# threads in one process as the benchmark runs them: ../stalls/wf-dir-stalls.cpp
# and wf-dir-mem-stalls.cpp, wf-dir.cpp and wf-dir-mem.cpp as they ship with the
# stalls of stall.hpp, and no stalls set, so that they time the reset of the
# arrays, which every thread waits for, apart from the search after it, RUNS
# runs each (5 by default) with the programs taking turns. For each run and
# thread count: the mean time of a search, its mean reset and search, when the
# last thread returned, and the workers' run delay (ready to run but without a
# core) and involuntary switches per search. Rows go to diag-phases.csv:
#   setsid nohup ./diag-phases.sh > diag-phases.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-5}
read -r -a programs <<< "${PROGRAMS:-wd mem}"
read -r -a graphs <<< "${GRAPHS:-lj twitter road uk kron hubs}"
build="$here/phases-build"
out="$here/diag-phases.csv"

for graph in "${graphs[@]}"; do
    set_graph "$graph" || { echo "unknown graph $graph" >&2; exit 1; }
    for file in "$path" "$ref"; do
        [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
    done
done

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T') on $(hostname)"

export TMPDIR="$build/tmp"
mkdir -p "$TMPDIR" "$build/run"
flags=(-std=c++23 -O3 -march=native -DNDEBUG -g -fno-omit-frame-pointer
       -falign-loops=32 -falign-jumps=32 -include "$root/benchmark/text-shift.h"
       -DTEXT_SHIFT=0 -pthread)
for name in "${programs[@]}"; do
    case $name in
        wd) "$cxx" "${flags[@]}" "$here/../stalls/wf-dir-stalls.cpp" -o "$build/wd.out" ;;
        mem) "$cxx" "${flags[@]}" "$here/../stalls/wf-dir-mem-stalls.cpp" -o "$build/mem.out" ;;
        *) echo "unknown program $name" >&2; exit 1 ;;
    esac
done
echo "built ${#programs[@]} programs"

cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

if [ ! -f "$out" ]; then
    echo "graph,program,run,threads,mean_us,reset_us,search_us,last_us,run_delay_us,involuntary,correct" > "$out"
fi
for run in $(seq 1 "$runs"); do
    for graph in "${graphs[@]}"; do
        set_graph "$graph"
        for i in "${!programs[@]}"; do
            name=${programs[(i + run) % ${#programs[@]}]}
            if awk -F, -v g="$graph" -v p="$name" -v r="$run" \
                    '$1 == g && $2 == p && $3 == r && $11 == "yes" { found = 1 } END { exit !found }' "$out"; then
                continue
            fi
            rm -f -- wfd-out.txt
            status=0
            env BFS_SPREAD_NODES=1 numactl --interleave=all \
                "$build/$name.out" "$path" 112,128 < /dev/null > stdout.txt 2> stderr.txt || status=$?
            correct=no
            if [ "$status" = 0 ] && cmp -s wfd-out.txt "$ref" &&
               awk '$1 == 128 && $3 == "same" { ok = 1 } END { exit !ok }' stdout.txt; then
                correct=yes
            fi
            # a thread count's sched line comes before its searches_us line, and
            # its last return and phases lines after
            awk -v graph="$graph" -v name="$name" -v run="$run" -v correct="$correct" '
                FNR == NR { if (NF == 3 && $1 ~ /^[0-9]+$/) { count[++counts] = $1; mean[$1] = $2 }; next }
                $1 == "sched" { c++; delay[c] = $3; invol[c] = $9 }
                $1 == "last" && $2 == "return" { last[c] = $3 }
                $1 == "phases" { reset[c] = $3; search[c] = $5 }
                END {
                    for (i = 1; i <= counts; i++)
                        printf "%s,%s,%d,%d,%d,%s,%s,%s,%s,%s,%s\n", graph, name, run, count[i],
                               mean[count[i]], reset[i], search[i], last[i], delay[i], invol[i], correct
                }' stdout.txt stderr.txt >> "$out"
            echo "$(date +%T) $graph $name run $run: $(tail -n 2 "$out" | cut -d, -f4-7 | tr '\n' ' ')correct $correct"
            if [ "$correct" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done
done
echo "done $(date '+%F %T')"

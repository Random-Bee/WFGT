#!/usr/bin/env bash
set -euo pipefail

# The experiment with a thread stalled at every level: LiveJournal, or the graph
# GRAPH names (twitter, road, uk, kron or hubs), from vertex 0 on THREADS
# threads spread over the NUMA nodes, with memory interleaved. THREADS is seven
# eighths of the CPUs unless
# set, 112 of the 128 of the paper's machine, which leaves the rest to the
# host and to the threads that stall the workers. In the stall builds of the
# programs (the *-stalls.cpp files here, with stall.hpp), each of the last
# BFS_STALLED threads busy-waits BFS_STALL_US microseconds when it starts its
# work in a level or round:
# wf-dir.cpp and wf-dir-mem.cpp at the top of every level, GAPBS at the start
# of every top-down and bottom-up step, GBBS and PASGAL at the first vertex or
# edge a worker takes in a round. Here the last of the threads stalls, for
# 0 to 10000 us, or on Road-USA, whose 6,262 levels each pay a stall in the
# programs that wait for it, for 0 to 1000 us; noise.sh stalls every thread, at
# random times.
# Each program is built once, with the benchmark's flags and its code unshifted.
# A run is 20 searches, and every setting runs RUNS times (5 by default) for
# each program; the programs take turns, so drift reaches them all alike. A run
# is correct when its distances match a sequential BFS's. Rows go to
# results/stalls-both.csv (stalls-<graph>.csv for another graph), and per setting the
# median of the runs to the matching -summary.csv; for our programs, last_us is when the last thread
# returned, which stalled threads put off. A stopped run picks up where it left
# off, as a run already correct is not run again. It runs for about half an hour:
#   setsid nohup ./stalls.sh > stalls.log 2>&1 &
# The compiler, where the graphs and their reference distances are, the lock
# and GAPBS's spin count come from config.sh.

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-5}
threads=${THREADS:-$((cpus - cpus / 8))}

case ${GRAPH:-lj} in
    lj) suffix=both ;;
    twitter|road|uk|kron|hubs) suffix=$GRAPH ;;
    *) echo "unknown graph ${GRAPH}" >&2; exit 1 ;;
esac
set_graph "${GRAPH:-lj}"
graph=$path
build="$here/build"
results="$here/results"
out="$results/stalls-$suffix.csv"
summary="$results/stalls-$suffix-summary.csv"
programs=(wd mem gapbs gbbs pasgal)
lengths="0 10 20 50 100 200 500 1000 2000 5000 10000"
[ "${GRAPH:-lj}" != road ] || lengths="0 10 20 50 100 200 500 1000"

# a setting a line: experiment, threads stalled, microseconds per stall
settings=$(
    for us in $lengths; do echo "length 1 $us"; done
)

[ -f "$graph" ] || { echo "missing $graph (BFS_DATASETS in config.sh)" >&2; exit 1; }
[ -f "$ref" ] || { echo "missing $ref: make-refs.sh ${GRAPH:-lj} writes it" >&2; exit 1; }

declare -A done_run
if [ -f "$out" ]; then
    while IFS=, read -r experiment stalled us run name time last correct finished; do
        if [ "$correct" = yes ]; then
            done_run["$experiment,$stalled,$us,$run,$name"]=1
        fi
    done < <(tail -n +2 "$out")
fi

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T') on $(hostname), $("$cxx" --version | awk 'NR == 1')," \
     "commit $(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo none)"

# GCC's intermediate files go here rather than in /tmp
export TMPDIR="$build/tmp"
mkdir -p "$TMPDIR" "$build/stall-run" "$results"
flags=(-std=c++23 -O3 -march=native -DNDEBUG -g -fno-omit-frame-pointer
       -falign-loops=32 -falign-jumps=32 -include "$root/benchmark/text-shift.h"
       -DTEXT_SHIFT=0)
parlay=(-mcx16 -pthread)
build_program() {
    local binary="$build/stall-$1.out"
    case $1 in
        wd) "$cxx" "${flags[@]}" -pthread "$here/wf-dir-stalls.cpp" -o "$binary" ;;
        mem) "$cxx" "${flags[@]}" -pthread "$here/wf-dir-mem-stalls.cpp" -o "$binary" ;;
        gapbs) "$cxx" "${flags[@]}" -fopenmp "$here/dir-bfs-stalls.cpp" -o "$binary" ;;
        pasgal) "$cxx" "${flags[@]}" "${parlay[@]}" \
                -I "$root/submodules/PASGAL/external/parlaylib/include" \
                "$here/pasgal-bfs-stalls.cpp" \
                -o "$binary" ;;
        gbbs) "$cxx" "${flags[@]}" "${parlay[@]}" -DPARLAY_USE_STD_ALLOC \
                -I "$root/submodules/parlaylib/include" "$here/gbbs-bfs-stalls.cpp" \
                -o "$binary" ;;
    esac
}
pids=()
for name in "${programs[@]}"; do
    build_program "$name" 2> "$TMPDIR/stall-build-$name.log" &
    pids+=($!)
done
for i in "${!pids[@]}"; do
    if ! wait "${pids[i]}"; then
        cat -- "$TMPDIR/stall-build-${programs[i]}.log" >&2
        exit 1
    fi
done
echo "built ${#programs[@]} programs"

if [ ! -f "$out" ]; then
    echo "experiment,stalled,stall_us,run,program,us,last_us,correct,finished" > "$out"
fi
cd "$build/stall-run"
cleanup() {
    rm -f -- wfd-out.txt dir-out.txt gbbs-out.txt pasgal-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

for run in $(seq 1 "$runs"); do
    while read -r experiment stalled us; do
        # a different program starts each run's settings
        for i in "${!programs[@]}"; do
            name=${programs[(i + run) % ${#programs[@]}]}
            key="$experiment,$stalled,$us,$run,$name"
            if [ -n "${done_run[$key]:-}" ]; then
                continue
            fi
            case $name in
                gapbs) output=dir-out.txt ;;
                gbbs) output=gbbs-out.txt ;;
                pasgal) output=pasgal-out.txt ;;
                *) output=wfd-out.txt ;;
            esac
            rm -f -- "$output"
            status=0
            # not named settings: that is the list each run reads its settings from
            run_env=(BFS_STALLED="$stalled" BFS_STALL_US="$us")
            [ "$name" != gapbs ] || run_env+=(GOMP_SPINCOUNT="$gapbs_spincount")
            env BFS_SPREAD_NODES=1 numactl --interleave=all env "${run_env[@]}" \
                "$build/stall-$name.out" "$graph" "$threads" < /dev/null > stdout.txt 2> stderr.txt ||
                status=$?
            # for one thread count a program prints just its time
            time=$(awk 'NF == 1 && $1 ~ /^[0-9]+$/ { t = $1 } END { print t }' stdout.txt)
            last=$(awk '$1 == "last" && $2 == "return" && $3 ~ /^[0-9]+$/ { t = $3 } END { print t }' \
                stderr.txt)
            correct=no
            if [ "$status" = 0 ] && [ -n "$time" ] && cmp -s "$output" "$ref"; then
                correct=yes
            fi
            echo "$key,${time:-failed},$last,$correct,$(date +%FT%T)" >> "$out"
            echo "$(date +%T) $key: ${time:-failed} us${last:+, last return $last us}, correct $correct"
            if [ "$correct" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done <<< "$settings"
done

# per setting and program, the median, fastest and slowest of the correct runs
# (a rerun's latest), the median last return for ours, and how many runs
{
    echo "experiment,stalled,stall_us,program,median_us,min_us,max_us,median_last_us,runs"
    awk -F, 'NR > 1 && $8 == "yes" { key = $1 "," $2 "," $3 "," $5
                                     t[key SUBSEP $4] = $6; l[key SUBSEP $4] = $7 }
             function median(v, k) { return (k % 2) ? v[(k + 1) / 2] : (v[k / 2] + v[k / 2 + 1]) / 2 }
             END { for (ks in t) { split(ks, p, SUBSEP)
                                   times[p[1]] = times[p[1]] " " t[ks]
                                   if (l[ks] != "") lasts[p[1]] = lasts[p[1]] " " l[ks] }
                   for (key in times) {
                       k = split(times[key], v, " "); asort(v)
                       m = split(lasts[key], w, " "); if (m) asort(w)
                       printf "%s,%d,%d,%d,%s,%d\n", key, median(v, k), v[1], v[k],
                              m ? sprintf("%d", median(w, m)) : "", k
                   } }' "$out" | sort -t, -k1,1 -k4,4 -k2,2n -k3,3n
} > "$summary"
cat -- "$summary"
echo "finished $(date '+%F %T')"

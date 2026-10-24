#!/usr/bin/env bash
set -euo pipefail

# why the programs slow down at 128 threads, where every core has a worker:
# the stall builds of ../stalls with no stalls set, on LiveJournal, or the
# graph GRAPH names (twitter, road, uk, kron or hubs), from
# vertex 0 over both sockets, at 112 and then 128 threads in one process as
# the benchmark runs them, RUNS times each (3 by default), the programs taking
# turns. For each run and thread count: the mean, first, median and slowest
# search; the workers' run delay per search (ready to run but without a core),
# its mean over the workers and its most for one, and their involuntary
# switches; for our programs, the reset, which waits for every thread, the
# search after it, and when the last thread returned; and for the others, in
# the same columns, the setup before their traversal and the traversal, the
# rest of a search coming after it. Rows go to diag128.csv,
# or diag128-<graph>.csv for another graph:
#   setsid nohup ./diag128.sh > diag128.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
bench="$root/benchmark"
stalls="$here/../stalls"
graph_name=${GRAPH:-lj}
set_graph "$graph_name" || { echo "unknown graph $graph_name" >&2; exit 1; }
graph=$path
suffix=""
[ "$graph_name" = lj ] || suffix="-$graph_name"
runs=${RUNS:-3}
read -r -a programs <<< "${PROGRAMS:-wd mem gapbs gbbs pasgal}"
build="$here/diag128-build"
out="$here/diag128$suffix.csv"

for file in "$graph" "$ref"; do
    [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
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
       -falign-loops=32 -falign-jumps=32 -include "$bench/text-shift.h"
       -DTEXT_SHIFT=0)
parlay=(-mcx16 -pthread)
build_program() {
    local binary="$build/$1.out"
    case $1 in
        wd) "$cxx" "${flags[@]}" -pthread "$stalls/wf-dir-stalls.cpp" -o "$binary" ;;
        mem) "$cxx" "${flags[@]}" -pthread "$stalls/wf-dir-mem-stalls.cpp" -o "$binary" ;;
        gapbs) "$cxx" "${flags[@]}" -fopenmp "$stalls/dir-bfs-stalls.cpp" -o "$binary" ;;
        pasgal) "$cxx" "${flags[@]}" "${parlay[@]}" \
                -I "$root/submodules/PASGAL/external/parlaylib/include" \
                "$stalls/pasgal-bfs-stalls.cpp" \
                -o "$binary" ;;
        gbbs) "$cxx" "${flags[@]}" "${parlay[@]}" -DPARLAY_USE_STD_ALLOC \
                -I "$root/submodules/parlaylib/include" "$stalls/gbbs-bfs-stalls.cpp" \
                -o "$binary" ;;
        *) echo "unknown program $1" >&2; return 1 ;;
    esac
}
pids=()
for name in "${programs[@]}"; do
    build_program "$name" 2> "$TMPDIR/build-$name.log" &
    pids+=($!)
done
for i in "${!pids[@]}"; do
    if ! wait "${pids[i]}"; then
        cat -- "$TMPDIR/build-${programs[i]}.log" >&2
        exit 1
    fi
done
echo "built ${#programs[@]} programs"

cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt dir-out.txt gbbs-out.txt pasgal-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

output_of() {
    case $1 in
        gapbs) echo dir-out.txt ;;
        gbbs) echo gbbs-out.txt ;;
        pasgal) echo pasgal-out.txt ;;
        *) echo wfd-out.txt ;;
    esac
}

echo "program,run,threads,mean_us,first_us,median_us,slowest_us,run_delay_us,most_run_delay_us,involuntary,reset_us,search_us,last_us,correct" > "$out"
for run in $(seq 1 "$runs"); do
    for name in "${programs[@]}"; do
        output=$(output_of "$name")
        rm -f -- "$output"
        settings=()
        [ "$name" != gapbs ] || settings=(GOMP_SPINCOUNT=300000)
        status=0
        env BFS_SPREAD_NODES=1 numactl --interleave=all env "${settings[@]}" \
            "$build/$name.out" "$graph" 112,128 < /dev/null > stdout.txt 2> stderr.txt || status=$?
        correct=no
        if [ "$status" = 0 ] && cmp -s "$output" "$ref" &&
           awk '$1 == 128 && $3 == "same" { ok = 1 } END { exit !ok }' stdout.txt; then
            correct=yes
        fi
        # a thread count's lines come in order: its sched and searches_us lines,
        # then for our programs its last return and phases lines
        awk -v name="$name" -v run="$run" -v correct="$correct" '
            FNR == NR { if (NF == 3 && $1 ~ /^[0-9]+$/) { count[++counts] = $1; mean[$1] = $2 }; next }
            $1 == "sched" { c++; delay[c] = $3; most[c] = $5; invol[c] = $9 }
            $1 == "searches_us" {
                n = NF - 1; first[c] = $2
                for (i = 1; i <= n; i++) v[i] = $(i + 1)
                asort(v); med[c] = n % 2 ? v[(n + 1) / 2] : (v[n / 2] + v[n / 2 + 1]) / 2; slow[c] = v[n]
                delete v
            }
            $1 == "last" && $2 == "return" { last[c] = $3 }
            $1 == "phases" { reset[c] = $3; search[c] = $5 }
            END {
                for (i = 1; i <= counts; i++)
                    printf "%s,%d,%d,%d,%d,%d,%d,%s,%s,%s,%s,%s,%s,%s\n", name, run, count[i], mean[count[i]],
                           first[i], med[i], slow[i], delay[i], most[i], invol[i],
                           (i in reset) ? reset[i] : "", (i in search) ? search[i] : "",
                           (i in last) ? last[i] : "", correct
            }' stdout.txt stderr.txt >> "$out"
        echo "$(date +%T) $name run $run: $(tail -n 2 "$out" | cut -d, -f3,4,5,6,7,8,10,11,12 | tr '\n' ' ') correct $correct"
    done
done
echo "done $(date '+%F %T')"

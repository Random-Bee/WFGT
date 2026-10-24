#!/usr/bin/env bash
set -euo pipefail

# where the time of GAPBS and of WF-DO goes, on the graphs of GRAPHS
# (LiveJournal, Twitter, Road-USA, uk-2002, Kronecker and the graph of hubs by
# default) from vertex 0 over both sockets, at the thread counts of THREADS
# (16, 64 and 112 by default) in one process as the benchmark runs them:
# dir-bfs-steps.cpp times every step of GAPBS, and per thread the start of each
# loop, the time on its share and the wait at its end, and wf-dir-steps.cpp
# times every level of WF-DO, and per thread its plan, codes, own share and
# helping. Their uninstrumented builds, ../../DO_BFS/dir-bfs.cpp and
# ../../WF_DO_BFS/wf-dir.cpp as the benchmark builds them with the code
# unshifted, run alongside to show what the timers cost. ROUNDS rounds (3 by
# default), the programs taking turns, and a round's runs that already have
# correct rows are skipped, so the script can resume. Per run and thread count,
# the time per search and whether the distances are the sequential BFS's go to
# steps.csv, and each timed build's report to logs/<graph>/<program>-r<round>.err,
# which summarize.py reads:
#   setsid nohup ./steps.sh > steps.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
rounds=${ROUNDS:-3}
threads=${THREADS:-16,64,112}
read -r -a graphs <<< "${GRAPHS:-lj twitter road uk kron hubs}"
read -r -a programs <<< "${PROGRAMS:-gapbs-steps wd-steps gapbs wd}"
build="$here/steps-build"
out="$here/steps.csv"
logs="$here/logs"

if [[ ! $threads =~ ^[0-9]+(,[0-9]+)*$ ]]; then
    echo "THREADS must be thread counts separated by commas" >&2
    exit 1
fi
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
       -DTEXT_SHIFT=0)
for name in "${programs[@]}"; do
    case $name in
        gapbs-steps) "$cxx" "${flags[@]}" -fopenmp "$here/dir-bfs-steps.cpp" -o "$build/$name.out" ;;
        wd-steps) "$cxx" "${flags[@]}" -pthread "$here/wf-dir-steps.cpp" -o "$build/$name.out" ;;
        gapbs) "$cxx" "${flags[@]}" -fopenmp "$root/DO_BFS/dir-bfs.cpp" -o "$build/$name.out" ;;
        wd) "$cxx" "${flags[@]}" -pthread "$root/WF_DO_BFS/wf-dir.cpp" -o "$build/$name.out" ;;
        *) echo "unknown program $name" >&2; exit 1 ;;
    esac
done
echo "built ${#programs[@]} programs"

cd "$build/run"
cleanup() {
    rm -f -- dir-out.txt wfd-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

if [ ! -s "$out" ]; then
    echo "round,graph,program,threads,us_per_search,correct,load" > "$out"
fi
for round in $(seq 1 "$rounds"); do
    for graph in "${graphs[@]}"; do
        set_graph "$graph"
        mkdir -p "$logs/$graph"
        for i in "${!programs[@]}"; do
            name=${programs[(i + round) % ${#programs[@]}]}
            if awk -F, -v r="$round" -v g="$graph" -v p="$name" \
                    '$1 == r && $2 == g && $3 == p && $6 == "yes" { found = 1 } END { exit !found }' "$out"; then
                continue
            fi
            settings=()
            case $name in
                gapbs*) output=dir-out.txt; settings=(GOMP_SPINCOUNT="$gapbs_spincount") ;;
                wd*) output=wfd-out.txt ;;
            esac
            # GAPBS is not run on Road-USA with every core taken
            if [ "$graph" = road ] && [[ $name == gapbs* ]] && [[ ",$threads," == *",128,"* ]]; then
                echo "skipping $name on road: THREADS includes 128"
                continue
            fi
            rm -f -- "$output"
            load=$(cut -d' ' -f1 /proc/loadavg)
            status=0
            env BFS_SPREAD_NODES=1 "${settings[@]}" numactl --interleave=all \
                "$build/$name.out" "$path" "$threads" < /dev/null > stdout.txt 2> stderr.txt || status=$?
            correct=no
            if [ "$status" = 0 ] && cmp -s "$output" "$ref" &&
               ! awk 'NF == 3 && $3 == "differs" { bad = 1 } END { exit !bad }' stdout.txt; then
                correct=yes
            fi
            if [[ $name == *-steps ]]; then
                command cp -- stderr.txt "$logs/$graph/$name-r$round.err"
            fi
            # given a single thread count, the programs print only its time
            awk -v r="$round" -v g="$graph" -v p="$name" -v c="$correct" -v l="$load" -v t="$threads" '
                NF == 3 && $1 ~ /^[0-9]+$/ { printf "%s,%s,%s,%s,%s,%s,%s\n", r, g, p, $1, $2, c, l }
                NF == 1 && $1 ~ /^[0-9]+$/ && t !~ /,/ { printf "%s,%s,%s,%s,%s,%s,%s\n", r, g, p, t, $1, c, l }
            ' stdout.txt >> "$out"
            echo "$(date +%T) round $round $graph $name: $(awk -F, -v r="$round" -v g="$graph" -v p="$name" \
                '$1 == r && $2 == g && $3 == p { printf "%s:%s ", $4, $5 }' "$out")correct $correct, load before $load"
            if [ "$correct" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done
done
echo "done $(date '+%F %T')"

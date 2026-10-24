#!/usr/bin/env bash
set -euo pipefail

# what it costs to copy a growing bucket's array with atomic loads: WF-DO and
# WF-DO-mem with the compiler barriers of the builds that the paper times
# (../ordering/wf-dir-barriers.cpp and wf-dir-mem-barriers.cpp, wd and mem),
# whose customList::reserve reads the array it copies with relaxed atomic
# loads, as the programs in ../../WF_DO_BFS do, against the same programs with
# the plain copy of those builds (wf-dir-plain-copy.cpp and
# wf-dir-mem-plain-copy.cpp, wd-plain and mem-plain), on the graphs of GRAPHS
# (LiveJournal, Twitter, Road-USA, uk-2002, Kronecker and the graph of hubs by
# default) from vertex 0 over both sockets, at the thread counts of THREADS
# (16, 64 and 112 by default) in one process as the benchmark runs them, with
# the code unshifted. ROUNDS rounds (3 by default), the programs taking turns,
# and a round's runs that already have correct rows are skipped, so the script
# can resume. Per run and thread count, the time per search and whether the
# distances are the sequential BFS's go to copy-times.csv:
#   setsid nohup ./copy-times.sh > copy-times.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
rounds=${ROUNDS:-3}
threads=${THREADS:-16,64,112}
read -r -a graphs <<< "${GRAPHS:-lj twitter road uk kron hubs}"
read -r -a programs <<< "${PROGRAMS:-wd wd-plain mem mem-plain}"
build="$here/copy-build"
out="$here/copy-times.csv"

if [[ ! $rounds =~ ^[1-9][0-9]*$ ]]; then
    echo "ROUNDS must be a positive number" >&2
    exit 1
fi
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
       -DTEXT_SHIFT=0 -pthread)
for name in "${programs[@]}"; do
    case $name in
        wd) source_file="$root/experiments/ordering/wf-dir-barriers.cpp" ;;
        mem) source_file="$root/experiments/ordering/wf-dir-mem-barriers.cpp" ;;
        wd-plain) source_file="$here/wf-dir-plain-copy.cpp" ;;
        mem-plain) source_file="$here/wf-dir-mem-plain-copy.cpp" ;;
        *) echo "unknown program $name" >&2; exit 1 ;;
    esac
    "$cxx" "${flags[@]}" "$source_file" -o "$build/$name.out"
done
echo "built ${#programs[@]} programs"

cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

if [ ! -s "$out" ]; then
    echo "round,graph,program,threads,us_per_search,correct,load" > "$out"
fi
for round in $(seq 1 "$rounds"); do
    for graph in "${graphs[@]}"; do
        set_graph "$graph"
        for i in "${!programs[@]}"; do
            name=${programs[(i + round) % ${#programs[@]}]}
            if awk -F, -v r="$round" -v g="$graph" -v p="$name" \
                    '$1 == r && $2 == g && $3 == p && $6 == "yes" { found = 1 } END { exit !found }' "$out"; then
                continue
            fi
            rm -f -- wfd-out.txt
            load=$(cut -d' ' -f1 /proc/loadavg)
            status=0
            env BFS_SPREAD_NODES=1 numactl --interleave=all \
                "$build/$name.out" "$path" "$threads" < /dev/null > stdout.txt 2> stderr.txt || status=$?
            correct=no
            if [ "$status" = 0 ] && cmp -s wfd-out.txt "$ref" &&
               ! awk 'NF == 3 && $3 == "differs" { bad = 1 } END { exit !bad }' stdout.txt; then
                correct=yes
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

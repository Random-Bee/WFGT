#!/usr/bin/env bash
set -euo pipefail

# The memory of WF-DO and WF-DO-mem: wf-dir-memory.cpp and wf-dir-mem-memory.cpp,
# copies of wf-dir.cpp and wf-dir-mem.cpp that report their memory, on the
# graphs of GRAPHS (LiveJournal, Twitter, Road-USA, uk-2002, Kronecker and the
# graph of hubs by default) from vertex 0, with 1, 16 and 112 threads
# spread over both NUMA nodes and memory interleaved. Each thread count runs in a process
# of its own, so that the peak resident size of a process is that of one thread
# count's 20 searches. A run gives the most bytes that the arenas handed out in a
# search (the arrays of buckets, and from arenas of their own the levels of
# WF-DO and the records of WF-DO-mem; its two nodes are counted apart), the
# bytes of the arenas' chunks, and the resident size before the searches and at
# their peak. A run is correct when its distances match a
# sequential BFS's. Rows go to memory-both.csv next to this script:
#   setsid nohup ./memory.sh > memory.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"

build="$here/memory-build"
out="$here/memory-both.csv"
programs=(wd mem)
read -r -a graphs <<< "${GRAPHS:-lj twitter road uk kron hubs}"
thread_counts=(1 16 112)

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
echo "started $(date '+%F %T') on $(hostname), $("$cxx" --version | awk 'NR == 1')"

# GCC's intermediate files go here rather than in /tmp
export TMPDIR="$build/tmp"
mkdir -p "$TMPDIR" "$build/run"
flags=(-std=c++23 -O3 -march=native -DNDEBUG -g -fno-omit-frame-pointer
       -falign-loops=32 -falign-jumps=32 -include "$root/benchmark/text-shift.h"
       -DTEXT_SHIFT=0 -pthread)
"$cxx" "${flags[@]}" "$here/wf-dir-memory.cpp" -o "$build/wd.out"
"$cxx" "${flags[@]}" "$here/wf-dir-mem-memory.cpp" -o "$build/mem.out"
echo "built ${#programs[@]} programs"

keys="search_bytes chunk_bytes node_bytes rss_before_kb rss_peak_kb"
echo "graph,threads,program,us,${keys// /,},correct,finished" > "$out"
cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt stdout.txt stderr.txt
}
trap cleanup EXIT

for graph in "${graphs[@]}"; do
    set_graph "$graph"
    for threads in "${thread_counts[@]}"; do
        for name in "${programs[@]}"; do
            rm -f -- wfd-out.txt
            status=0
            env BFS_SPREAD_NODES=1 numactl --interleave=all "$build/$name.out" "$path" "$threads" \
                < /dev/null > stdout.txt 2> stderr.txt || status=$?
            time=$(awk 'NF == 1 && $1 ~ /^[0-9]+$/ { t = $1 } END { print t }' stdout.txt)
            fields=$(awk -v keys="$keys" '
                $1 == "memory" { for (i = 2; i < NF; i += 2) c[$i] = $(i + 1) }
                END { n = split(keys, k, " ")
                      for (i = 1; i <= n; i++) printf "%s%s", (i > 1 ? "," : ""), c[k[i]] }' \
                stderr.txt)
            correct=no
            if [ "$status" = 0 ] && [ -n "$time" ] && cmp -s wfd-out.txt "$ref"; then
                correct=yes
            fi
            echo "$graph,$threads,$name,${time:-failed},$fields,$correct,$(date +%FT%T)" >> "$out"
            echo "$(date +%T) $graph $threads $name: ${time:-failed} us, $fields, correct $correct"
            if [ "$correct" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done
done
echo "finished $(date '+%F %T')"

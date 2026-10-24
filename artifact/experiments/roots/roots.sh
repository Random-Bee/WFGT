#!/usr/bin/env bash
set -euo pipefail

# The five programs of benchmark/benchmark.sh from roots other than vertex 0, on
# the graphs of GRAPHS (LiveJournal, Twitter, Road-USA, uk-2002, Kronecker and
# the graph of hubs by default), with 16 and 112 threads split evenly over both NUMA
# nodes and memory interleaved over both. The roots are drawn uniformly at random with a
# fixed seed (SEED, 1 by default), and a root is kept if a search from it reaches
# at least half of the vertices, until there are ROOTS of them (16 by default);
# vertex 0, the root of the benchmark, runs too. The sequential baseline gives
# every root's reference distances, kept under roots/. Each program is built
# once, with the benchmark's flags and its code unshifted, and runs 20 searches
# from a root per thread count; the programs take turns from root to root. A run
# is correct when its distances match the reference. Rows go to roots-both.csv next
# to this script, and per graph, program and thread count the median, mean,
# fastest and slowest over the random roots to roots-both-summary.csv. A stopped run picks up
# where it left off:
#   setsid nohup ./roots.sh > roots.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
seed=${SEED:-1}
wanted=${ROOTS:-16}
counts=16,112

build="$here/roots-build"
root_refs="$here/roots"
out="$here/roots-both.csv"
summary="$here/roots-both-summary.csv"
programs=(wd mem gapbs gbbs pasgal)
read -r -a graphs <<< "${GRAPHS:-lj twitter road uk kron hubs}"

# sets path, the graph's file, and base, the distances from vertex 0
set_graph_base() {
    set_graph "$1" || { echo "unknown graph $1" >&2; return 1; }
    base=$ref
}

output_of() {
    case $1 in
        gapbs) echo dir-out.txt ;;
        gbbs) echo gbbs-out.txt ;;
        pasgal) echo pasgal-out.txt ;;
        seq) echo dir-seq-out.txt ;;
        *) echo wfd-out.txt ;;
    esac
}

for graph in "${graphs[@]}"; do
    set_graph_base "$graph"
    for file in "$path" "$base"; do
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
mkdir -p "$TMPDIR" "$build/run" "$root_refs"
flags=(-std=c++23 -O3 -march=native -DNDEBUG -g -fno-omit-frame-pointer
       -falign-loops=32 -falign-jumps=32 -include "$root/benchmark/text-shift.h"
       -DTEXT_SHIFT=0)
parlay=(-mcx16 -pthread)
build_program() {
    local binary="$build/$1.out"
    case $1 in
        wd) "$cxx" "${flags[@]}" -pthread "$root/WF_DO_BFS/wf-dir.cpp" -o "$binary" ;;
        mem) "$cxx" "${flags[@]}" -pthread "$root/WF_DO_BFS/wf-dir-mem.cpp" -o "$binary" ;;
        gapbs) "$cxx" "${flags[@]}" -fopenmp "$root/DO_BFS/dir-bfs.cpp" -o "$binary" ;;
        pasgal) "$cxx" "${flags[@]}" "${parlay[@]}" \
                -I "$root/submodules/PASGAL/external/parlaylib/include" \
                "$root/PASGAL_BFS/pasgal-bfs.cpp" \
                -o "$binary" ;;
        gbbs) "$cxx" "${flags[@]}" "${parlay[@]}" -DPARLAY_USE_STD_ALLOC \
                -I "$root/submodules/parlaylib/include" "$root/GBBS_BFS/gbbs-bfs.cpp" \
                -o "$binary" ;;
        seq) "$cxx" "${flags[@]}" -pthread "$root/DO_BFS/dir-bfs-seq.cpp" -o "$binary" ;;
    esac
}
pids=()
names=("${programs[@]}" seq)
for name in "${names[@]}"; do
    build_program "$name" 2> "$TMPDIR/build-$name.log" &
    pids+=($!)
done
for i in "${!pids[@]}"; do
    if ! wait "${pids[i]}"; then
        cat -- "$TMPDIR/build-${names[i]}.log" >&2
        exit 1
    fi
done
echo "built ${#names[@]} programs"

cd "$build/run"
cleanup() {
    rm -f -- wfd-out.txt dir-out.txt gbbs-out.txt pasgal-out.txt dir-seq-out.txt \
        stdout.txt stderr.txt
}
trap cleanup EXIT

# a reference's vertices reached and levels: its lines are the distances, a
# blank line, and the largest distance
reach_of() {
    awk 'NF == 0 { exit } $1 >= 0 { n++ } $1 > top { top = $1 } END { print n + 0, top + 1 }' "$1"
}

# the reference from root $2 of graph $1, made once
make_reference() {
    local ref="$root_refs/$1-$2.txt"
    if [ ! -f "$ref" ]; then
        rm -f -- dir-seq-out.txt
        numactl --cpunodebind=0 --membind=0 env BFS_SOURCE="$2" "$build/seq.out" "$path" \
            < /dev/null > stdout.txt 2> stderr.txt
        command mv -f -- dir-seq-out.txt "$ref"
    fi
}

# the roots of each graph: vertex 0, then the random ones, chosen once
for graph in "${graphs[@]}"; do
    set_graph_base "$graph"
    list="$root_refs/$graph-roots.txt"
    if [ -f "$list" ]; then
        continue
    fi
    n=$(( $(wc -l < "$base") - 2 ))
    make_reference "$graph" 0
    read -r reached levels <<< "$(reach_of "$root_refs/$graph-0.txt")"
    echo "0 $reached $levels" > "$list.part"
    kept=0
    for candidate in $(python3 -c 'import random, sys
r = random.Random(int(sys.argv[1]))
print(" ".join(str(r.randrange(int(sys.argv[2]))) for _ in range(1000)))' "$seed" "$n"); do
        [ "$candidate" != 0 ] || continue
        if awk -v c="$candidate" '$1 == c { found = 1 } END { exit !found }' "$list.part"; then
            continue
        fi
        make_reference "$graph" "$candidate"
        read -r reached levels <<< "$(reach_of "$root_refs/$graph-$candidate.txt")"
        if [ $(( 2 * reached )) -ge "$n" ]; then
            echo "$candidate $reached $levels" >> "$list.part"
            kept=$((kept + 1))
            echo "$(date +%T) $graph: root $candidate reaches $reached of $n in $levels levels"
        else
            echo "$(date +%T) $graph: root $candidate reaches only $reached of $n, skipped"
            rm -f -- "$root_refs/$graph-$candidate.txt"
        fi
        [ "$kept" -lt "$wanted" ] || break
    done
    command mv -f -- "$list.part" "$list"
done

declare -A done_run
if [ -f "$out" ]; then
    while IFS=, read -r graph source reached levels name threads us correct finished; do
        if [ "$correct" = yes ]; then
            done_run["$graph,$source,$name,$threads"]=1
        fi
    done < <(tail -n +2 "$out")
else
    echo "graph,root,reached,levels,program,threads,us,correct,finished" > "$out"
fi

for graph in "${graphs[@]}"; do
    set_graph_base "$graph"
    index=0
    while read -r source reached levels; do
        index=$((index + 1))
        ref="$root_refs/$graph-$source.txt"
        # a different program starts at each root
        for i in "${!programs[@]}"; do
            name=${programs[(i + index) % ${#programs[@]}]}
            if [ -n "${done_run[$graph,$source,$name,16]:-}" ] &&
                    [ -n "${done_run[$graph,$source,$name,112]:-}" ]; then
                continue
            fi
            output=$(output_of "$name")
            rm -f -- "$output"
            status=0
            settings=(BFS_SOURCE="$source")
            [ "$name" != gapbs ] || settings+=(GOMP_SPINCOUNT=300000)
            env BFS_SPREAD_NODES=1 numactl --interleave=all env "${settings[@]}" \
                "$build/$name.out" "$path" "$counts" < /dev/null > stdout.txt 2> stderr.txt ||
                status=$?
            ok=no
            if [ "$status" = 0 ] && cmp -s "$output" "$ref"; then
                ok=yes
            fi
            when=$(date +%FT%T)
            rows=$(awk -v g="$graph" -v s="$source" -v r="$reached" -v l="$levels" -v p="$name" \
                       -v ok="$ok" -v when="$when" '
                NF == 3 && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {
                    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", g, s, r, l, p, $1, $2,
                           (ok == "yes" && $3 == "same") ? "yes" : "no", when }' stdout.txt)
            if [ -z "$rows" ]; then
                rows="$graph,$source,$reached,$levels,$name,16,failed,no,$when
$graph,$source,$reached,$levels,$name,112,failed,no,$when"
            fi
            echo "$rows" >> "$out"
            echo "$(date +%T) $graph root $source $name: $(echo "$rows" | awk -F, '{ printf "%s:%s ", $6, $7 }')correct $ok"
            if [ "$ok" != yes ]; then
                echo "    exit status $status"
                tail -n 5 stderr.txt | sed 's/^/    /'
            fi
        done
    done < "$root_refs/$graph-roots.txt"
done

# per graph, program and thread count, over the random roots: the median,
# mean, fastest and slowest time, and the time from vertex 0
{
    echo "graph,program,threads,roots,median_us,mean_us,min_us,max_us,root0_us"
    awk -F, 'NR > 1 && $8 == "yes" { key = $1 "," $5 "," $6
                                     if ($2 == 0) zero[key] = $7
                                     else { t[key] = t[key] " " $7; sum[key] += $7; seen[key] = 1 } }
             END { for (key in seen) {
                       k = split(t[key], v, " "); asort(v)
                       m = (k % 2) ? v[(k + 1) / 2] : (v[k / 2] + v[k / 2 + 1]) / 2
                       printf "%s,%d,%d,%d,%d,%d,%s\n", key, k, m, sum[key] / k, v[1], v[k], zero[key] } }' "$out" |
        sort -t, -k1,1 -k3,3n -k2,2
} > "$summary"
cat -- "$summary"
echo "finished $(date '+%F %T')"

#!/usr/bin/env bash
set -euo pipefail

# GAPBS with OpenMP's default wait policy and with OMP_WAIT_POLICY=active,
# against WF-DO, on the graphs as the benchmark loads them (GRAPHS: LiveJournal,
# Twitter, the road network, uk-2002, Kronecker and the graph of hubs by
# default), RUNS times (1 by
# default), with the -O3 builds benchmark.sh made (benchmark/build). PLACEMENT
# is node0, threads and memory on NUMA node 0, or both, threads spread evenly
# over both nodes and memory interleaved, as benchmark.sh places them; COUNTS
# are the thread counts, 16,32,56 by default. Each run's output is kept in gapbs-active/, and the
# times go to gapbs-active/summary-PLACEMENT.txt:
#   setsid nohup ./gapbs-active.sh > gapbs-active.log 2>&1 &

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source "$here/../../benchmark/config.sh"
runs=${RUNS:-1}
read -r -a graphs <<< "${GRAPHS:-lj twitter road uk kron hubs}"
placement=${PLACEMENT:-node0}
counts=${COUNTS:-16,32,56}
case $placement in
    node0) launcher=(numactl --cpunodebind=0 --membind=0); wd_binary=wd-0.out ;;
    both) launcher=(env BFS_SPREAD_NODES=1 numactl --interleave=all); wd_binary=wd-0.out ;;
    *) echo "unknown placement $placement" >&2; exit 1 ;;
esac
bench="$root/benchmark/build"
dir="$here/gapbs-active"
configs=(gapbs gapbs-active wd)

set_graph() {
    case $1 in
        lj) path="$datasets/liveJournal1.edges"; ref="$refs/seq-liveJournal1.txt" ;;
        twitter) path="$datasets/twitter-2010.txt"; ref="$refs/twitter-ref.txt" ;;
        road) path="$datasets/road-road-usa.mtx"; ref="$refs/road-reference.txt" ;;
        uk) path="$datasets/uk-2002.mtx"; ref="$refs/uk-2002-ref.txt" ;;
        kron) path="$datasets/kron-25.edges"; ref="$refs/kron-25-ref.txt" ;;
        hubs) path="$root/datasets/hubs.edges"; ref="$refs/hubs-ref.txt" ;;
        *) echo "unknown graph $1" >&2; exit 1 ;;
    esac
}

for graph in "${graphs[@]}"; do
    set_graph "$graph"
    for file in "$path" "$ref"; do
        [ -f "$file" ] || { echo "missing $file" >&2; exit 1; }
    done
done
for binary in gapbs-0.out "$wd_binary"; do
    [ -f "$bench/$binary" ] || { echo "missing $bench/$binary" >&2; exit 1; }
done

mkdir -p "$(dirname "$lock")"
exec 9> "$lock"
if ! flock -n 9; then
    echo "waiting for $lock, held by another benchmark"
    flock 9
fi
echo "started $(date '+%F %T'), placement $placement, threads $counts"

mkdir -p "$dir/run"
cd "$dir/run"
trap 'rm -f -- dir-out.txt wfd-out.txt' EXIT
for graph in "${graphs[@]}"; do
    set_graph "$graph"
    for run in $(seq 1 "$runs"); do
        for name in "${configs[@]}"; do
            settings=(BFS_NOISE_US=0)
            case $name in
                gapbs) binary=gapbs-0.out; output=dir-out.txt ;;
                gapbs-active) binary=gapbs-0.out; output=dir-out.txt; settings=(OMP_WAIT_POLICY=active) ;;
                wd) binary=$wd_binary; output=wfd-out.txt ;;
            esac
            log="$dir/$placement-$graph-$name-$run"
            rm -f -- "$output"
            status=0
            "${launcher[@]}" env "${settings[@]}" "$bench/$binary" "$path" \
                "$counts" < /dev/null > "$log.out" 2> "$log.err" || status=$?
            correct=no
            if [ "$status" = 0 ] && cmp -s "$output" "$ref"; then
                correct=yes
            fi
            echo "$(date +%T) $graph $name run $run: $(awk '{ printf "%s:%.1f ", $1, $2 / 1000 }' "$log.out")correct $correct"
        done
    done
done

# per graph and configuration, the median over the runs at each thread count, in ms
for graph in "${graphs[@]}"; do
    for name in "${configs[@]}"; do
        cat -- "$dir/$placement-$graph-$name"-[0-9]*.out | awk -v g="$graph" -v name="$name" \
            -v counts="$counts" '
            { t[$1] = t[$1] " " $2 }
            END { line = sprintf("%-8s %-13s", g, name)
                  n = split(counts, c, ",")
                  for (i = 1; i <= n; i++) { k = split(t[c[i]], v, " "); asort(v)
                      m = (k % 2) ? v[(k + 1) / 2] : (v[k / 2] + v[k / 2 + 1]) / 2
                      line = line sprintf(" %3s:%8.1f", c[i], m / 1000) }
                  print line }'
    done
done > "$dir/summary-$placement.txt"
cat -- "$dir/summary-$placement.txt"
echo "finished $(date '+%F %T')"

# Settings that depend on the machine, for the scripts that build and run the
# programs: the scripts in benchmark/ and in the folders of experiments/ source
# this file. A setting comes from the environment when it is set there, and
# otherwise from benchmark/local.sh, which git ignores so that each machine can
# keep its own, with lines such as
#   : "${BFS_CXX:=/opt/rh/gcc-toolset-15/root/usr/bin/g++}"
# that leave the environment the last word. Anything still unset takes the
# default below.
#   BFS_CXX          a C++23 compiler, GCC 11 or later: g++
#   BFS_DATASETS     the directory of the downloaded graphs, LiveJournal,
#                    Twitter, Road-USA, uk-2002 and Kronecker: datasets/ next
#                    to benchmark/, where the graphs that graph_helpers
#                    generates go too
#   BFS_REFS         the directory of each graph's distances from the
#                    sequential BFS, which make-refs.sh writes and every run is
#                    checked against: refs/ next to benchmark/
#   BFS_LOCK         the lock that keeps two benchmarks off the machine at once:
#                    benchmark/build/bench.lock
#   GAPBS_SPINCOUNT  GOMP_SPINCOUNT for GAPBS, how many times a thread waiting
#                    at a barrier spins before it sleeps: 300000, about 3 ms on
#                    the Xeon Platinum 8592V of the paper's runs. A spin takes
#                    far less time on CPUs whose PAUSE instruction is shorter,
#                    such as AMD's Zen 2, so set it there to spin about as long
#   BFS_PERF         the perf binary that experiments/caches/road-mem-perf.sh
#                    counts events with: perf

config_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(dirname "$config_dir")
if [ -f "$config_dir/local.sh" ]; then
    # shellcheck source=/dev/null
    source "$config_dir/local.sh"
fi
cxx=${BFS_CXX:-g++}
datasets=${BFS_DATASETS:-$root/datasets}
refs=${BFS_REFS:-$root/refs}
lock=${BFS_LOCK:-$config_dir/build/bench.lock}
gapbs_spincount=${GAPBS_SPINCOUNT:-300000}
perf=${BFS_PERF:-perf}

# the CPUs this process may run on, and those of NUMA node 0
cpus=$(nproc)
cpus_node0=$cpus
if [ -r /sys/devices/system/node/node0/cpulist ]; then
    cpus_node0=$(awk -F, '{ n = 0
                            for(i = 1; i <= NF; i++) n += split($i, r, "-") == 2 ? r[2] - r[1] + 1 : 1
                            print n }' /sys/devices/system/node/node0/cpulist)
fi

# the thread counts given that fit in at most $1 CPUs, then $1 itself if it is
# not among them
counts_upto() {
    local most=$1 t
    local kept=()
    shift
    for t in "$@"; do
        if (( t <= most )); then kept+=("$t"); fi
    done
    [[ " ${kept[*]} " == *" $most "* ]] || kept+=("$most")
    echo "${kept[*]}"
}

# sets path, the graph's file, and ref, its distances from the sequential BFS
set_graph() {
    case $1 in
        lj) path="$datasets/liveJournal1.edges"; ref="$refs/seq-liveJournal1.txt" ;;
        twitter) path="$datasets/twitter-2010.txt"; ref="$refs/twitter-ref.txt" ;;
        road) path="$datasets/road-road-usa.mtx"; ref="$refs/road-reference.txt" ;;
        uk) path="$datasets/uk-2002.mtx"; ref="$refs/uk-2002-ref.txt" ;;
        kron) path="$datasets/kron-25.edges"; ref="$refs/kron-25-ref.txt" ;;
        hubs) path="$root/datasets/hubs.edges"; ref="$refs/hubs-ref.txt" ;;
        grid) path="$root/datasets/grid.edges"; ref="$refs/grid-ref.txt" ;;
        ljtail) path="$root/datasets/lj-tail.edges"; ref="$refs/lj-tail-ref.txt" ;;
        chain) path="$root/datasets/chain.edges"; ref="$refs/chain-ref.txt" ;;
        *) return 1 ;;
    esac
}

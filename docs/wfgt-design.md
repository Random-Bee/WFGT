# WFGT: design note

WFGT (wait-free graph traversal) is a library implementation of WF-DO-mem, the
variant of the paper's method that keeps each thread's part of a level in two
reused nodes and adds only a record per level, so that its memory depends on
the largest levels of a search rather than on all of them. In the reruns on
the idle machine it is within about 5% of WF-DO on the graphs of low
diameter, and faster on Road-USA and the graph of hubs, so the library has no
other variant. It keeps the variant's shared-memory protocol, so the paper's
proofs of correctness and wait-freedom apply to it, but it is not the code
the paper measured: it aims to be as fast as it can be, and is free to use
SIMD, prefetching and whatever later optimizations keep the protocol (see
"Protocol invariants").

The paper's code lives on in `artifact/` (see "Preserving the paper version").
The two never share code, so no change to the library can change what the
paper's numbers come from.

## Repository layout

```text
artifact/           the paper version: what every table and figure comes from
  README.md         environment, how to run, and each table and figure mapped
                    to its script, build and data file
  WF_BFS/           wf-bfs.cpp, the method with top-down levels only
                    (Section 3), which WF-DO extends
  WF_DO_BFS/        wf-dir.cpp (WF-DO) and wf-dir-mem.cpp (WF-DO-mem), as the
                    main comparison builds them
  DO_BFS/           the GAPBS port (dir-bfs.cpp) and the sequential baseline
  GBBS_BFS/, PASGAL_BFS/   drivers around the upstream kernels
  submodules/       upstream code at the commits used: gapbs/, gbbs/, PASGAL/,
                    and parlaylib/, the ParlayLib that the GBBS builds use
  cpu_helpers/, graph_helpers/   pinning, the graph reader, graph generators
  benchmark/        benchmark.sh (the main comparison), make-refs.sh, config.sh
  experiments/      a folder for each of the other experiments (stalls/,
                    work/, chunks/, memory/, helping/ and others), with its
                    scripts and a file for each configuration it builds
  results/          the data files the paper's tables and figures come from
  paper/            the paper's LaTeX source
wfgt/               the library
  include/wfgt/     header-only library
  tests/            correctness and robustness tests
  bench/            the library's own timing driver
docs/               this note
```

Inside `artifact/` the directories keep the names they had while the
experiments ran, except `benchmark/`, `cpu_helpers/` and `graph_helpers/`,
lowercased since; the scripts and includes use the new names, which leave the
compiled code as it was. No source there has switches
that choose code: the main programs are what the main comparison built, and
each other experiment has a file of its own, so what a result was measured
with can be read directly. `tmp/` at the top is a scratch directory that git
ignores; nothing in either part depends on it.

## Preserving the paper version

- **What `artifact/` holds:** the programs of the main comparison,
  the method with top-down levels only that WF-DO extends, a file for each
  configuration of the other experiments (stalls, counts, chunks, orders,
  memory, and the direction-rule, marking and helping variants), the baseline
  drivers, the upstream code at its commits, every script, and the data files
  behind each table and figure. Its README maps each table and figure to its
  script, source files and data file, and records the compiler, flags,
  machine, the graphs' sources and checksums, and the generators' parameters.
- **Freeze:** once the pending reruns are in, tag the commit (`paper-v1`) and
  archive it with a DOI (Zenodo or Software Heritage; ACM's "Artifacts
  Available" badge requires such an archive). The paper cites the DOI, and
  says that WFGT is a separate, faster implementation of the same method
  whose timings are not those of the tables.
- **After the freeze:** `artifact/` takes only build fixes for newer
  compilers and documented bug fixes, each noted in its README with its effect
  on results, and never optimizations. Later papers that need the stall,
  count or chunk builds work from it.
- **Why a copy on the main branch as well as the tag:** the instrumentation
  survives only there, since the library has none, and one checkout can time
  the library against the paper version.

## Library goals

- Exact distances, and a wait-free call: `participate` returns after a bounded
  number of its own steps, whatever the other threads do.
- As fast as possible on x86-64, and never slower than the paper's WF-DO-mem
  at any thread count on the paper's graphs (see "Performance checks").
- No experiment options: no stall points, counters, timing copies or
  switches between variants. Hub chunks are always on.
- No global state: several solvers, for different graphs, can live in one
  process.
- Usable from threads the caller owns, as well as from a pinned thread pool.

Not in the first version: other traversals, weighted or directed graphs,
targets other than x86-64 (see "Memory model"), graph I/O, and thread pinning
outside Linux.

## Public API (sketch)

```cpp
namespace wfgt {

using vertex = std::int32_t;   // the paper's code uses int ids and size_t offsets
using offset = std::size_t;

// a symmetric graph in CSR form: u's neighbors are neighbors[offsets[u]] up to
// neighbors[offsets[u+1]]. The library never copies or owns it
struct csr_graph {
    std::span<const offset> offsets;    // num_vertices() + 1 entries
    std::span<const vertex> neighbors;  // offsets.back() entries
    vertex num_vertices() const { return vertex(offsets.size() - 1); }
};

struct options {
    int threads = 1;
    std::vector<int> thread_nodes;      // NUMA node of each thread; empty: one node
    long long alpha = 15, beta = 18;    // GAPBS's direction thresholds
};

class bfs {
public:
    bfs(const csr_graph& graph, const options& opts);   // allocates everything once

    // between searches, not wait-free. Every call of participate() of the last
    // search must have returned, since a late thread still reads that state
    void prepare(vertex source);                    // one thread resets everything
    void prepare_share(vertex source, int thread);  // or each thread its share; the
                                                    // caller synchronizes them
                                                    // before any participate()

    // wait-free: returns after a bounded number of its own steps, with every
    // distance final, whatever the other threads do
    void participate(int thread);

    std::span<const std::int32_t> distances() const;    // -1: unreachable
};

// persistent threads, pinned to the given CPUs, that run prepare_share and
// participate; run() returns when the first thread returns, and waits for the
// rest of the last search before it starts another. thread_ids() gives the
// workers' kernel thread ids, which the robustness tests stall from outside
class bfs_pool {
public:
    bfs_pool(const csr_graph& graph, std::span<const int> cpus);
    void run(vertex source);
    std::span<const std::int32_t> distances() const;
    std::span<const pid_t> thread_ids() const;
};

} // namespace wfgt
```

`options::thread_nodes` replaces the NUMA lookup that `build_help_order`
makes in the paper's code; the optional `wfgt/platform/affinity.hpp` fills it
from sysfs and pins threads. `alpha` and `beta` are options because they
decide the direction of levels, as GAPBS's flags do, and their best values
depend on the graph; the sizes of work units are not (see "Tuning
constants").

## Tuning constants

The paper's code fixes a few sizes that trade work against overhead. None of
them affects correctness, since the protocol holds for any positive value,
so they stay out of the public API. There, each would need validation and
tests over its whole range, and would fix the current structure in place:
a SIMD bottom-up step, or chunks that adapt to degree, would change what
they mean. They live in one struct, `detail::tuning`, read at run time with
the paper's values as defaults. Tests and the timing driver set them through
a constructor in `detail`, and one becomes an option only if measurements
show that users need to change it.

| Constant | Default | What it trades |
|---|---|---|
| `hub_chunk` | 256 neighbors | certificates written, against neighbors that helpers on a hub examine again (`artifact/experiments/chunks/` can time other sizes) |
| `bottom_up_block` | 1024 vertices | finer balance and helping, against restarts of the hardware prefetcher at every block |
| `ordered_list_max` | 3 neighbors | the longest list a helper walks in order, saving two random draws |

Reading them at run time costs a compare per expanded vertex and a division
per hub, and the bottom-up step's buffer of a block's candidates is sized at
construction. Tests set them small, so that small graphs reach hub chunks and
many blocks per thread. Fixed, since they are layout or format rather than
tuning: the 128-byte alignment and padding of shared state, the arenas'
4 MiB chunks, a bucket's first array of one arena block and its doubling, and
one-byte frontier codes.

## Internal structure

Header-only, in `wfgt/include/wfgt/`. The first version maps each piece of the
paper's code (`artifact/WF_DO_BFS/wf-dir-mem.cpp`) to one header;
optimizations then change the headers freely within the protocol invariants:

| Header | From the paper's code |
|---|---|
| `detail/shared.hpp` | `shared_load`, `shared_store`, `acquire_load`, `release_store` |
| `detail/arena.hpp` | `arena`, two per thread: one that holds only bucket arrays, so its memory only ever holds ints, and one for records |
| `detail/bucket.hpp` | `customList` |
| `detail/affine.hpp` | `affinePermutation`, `affine_shuffle`, `next_affine_index` |
| `detail/help_order.hpp` | `help_order`, `near_owners`, `ownerWalk` |
| `detail/tuning.hpp` | the constants of "Tuning constants" |
| `detail/plan.hpp` | `level_plan`, the direction rule, frontier codes (`assign_code`) |
| `detail/levels.hpp` | the two reused nodes, `level_record` and its slot, `decide`, `read_frontier`, `plan_for`, level-tagged buckets, `last_done_on`, `at_level` |
| `detail/top_down.hpp` | own and other buckets, shuffled lists, hub chunks |
| `detail/bottom_up.hpp` | owned blocks, `helped_at`, frontier tests, the frontier-code pass |
| `bfs.hpp`, `pool.hpp` | the API above |
| `platform/affinity.hpp` | pinning and NUMA nodes, optional |

### State

- **Per solver**, where the paper's code has globals: the graph view, `dist`,
  `dist8`, `expanded`, the chunk certificates, `helped_at`, the block counts,
  the help order, the two nodes, the first level's slot, the arenas, the
  tuning constants and the search number.
- **Per thread**, where it has `thread_local`s and loose locals: the two
  arenas, the random generator, the spare record, the chunk stamp and the buffer
  of a block's candidates, in one struct aligned to 128 bytes, in an array
  indexed by thread.
- **Search control** (the number to run, the reset count, return times) lives
  in `bfs_pool`; `bfs` itself never waits.

### Levels

Each thread's bucket and progress live in two nodes, one for the even levels
and one for the odd, and a thread reuses its part of a node for level L+2
once level L+1 is chosen. Each level also gets a record from the record arenas,
holding its plan and the slot for the next level's record, an atomic pointer
that starts out null; the first level's slot belongs to the solver. Every
thread that finds a level's slot empty proposes a record with its plan, and
the first to install its record by CAS decides for all (`decide`). A level is
over once its record's slot is set, which threads test between owners to
leave a level early; a thread that has fallen behind walks the records,
taking each plan, until it reaches a level that is not over. A record whose
plan has an empty frontier ends the search. A losing record is kept for the
thread's next try, so a search allocates O(D + T) of them, 256 bytes each.
Buckets carry their level and done flags, and a thread that has fallen
behind checks them: `last_done_on` trusts an owner's progress only if its
next bucket has the next level, and `at_level` keeps only entries whose
distance is the reader's level. WF-DO's levels, a
fresh node per level linked by a CAS of its own, stay in
`artifact/WF_DO_BFS/wf-dir.cpp`.

## Protocol invariants

The proofs cover the library only while these hold. Every optimization is
checked against this list before it is measured:

1. **Work before its certificate.** Every write that lets another thread skip
   work comes after the work it covers: an append before the mark, a code
   before the mark, an array before its size, a chunk's discoveries before its
   certificate, an entry's or block's work before its progress and done flag,
   and a reset of progress before the bucket's new tag.
2. **Ordered reads stay ordered.** A tag before the size it guards, a size
   before the array, a level's choice before its plan.
3. **One shared access stays one access.** Each read or write of shared data
   that the proofs count (distances, codes, `expanded`, sizes, arrays,
   progress, flags, stamps, certificates) is a single atomic access of its own
   width. It is not split, repeated, or merged into a wider read-modify-write.
4. **No store writes bytes it does not own.** A vector store into a shared
   array writes only its own lanes (a masked store), never a whole vector
   whose other lanes hold values read earlier, since that can undo another
   thread's write.
5. **Wait-free loops.** Every loop runs over a range fixed when it starts,
   each CAS is tried once, and a test of another thread's progress only ends
   a loop early.
6. **Final values only.** A distance or code is written only with its final
   value.
7. **No allocation in a search** beyond the per-thread arenas, and no
   exceptions or I/O.

Free to change: anything that reads only the graph or a thread's private
state, the order of independent work, prefetching, instruction selection,
the layout of private data, and the layout of shared data as long as the
padding rules below hold.

- Each thread's frequently written shared state (bucket size and degree sum,
  progress) stays on cache lines no other thread writes, with two lines of
  padding where the L2 prefetcher pairs lines.
- Hot loops copy array pointers into locals, such as
  `const int* dist = state.dist`, so that moving arrays into the solver adds
  no reloads after atomic stores, across which GCC reuses no load of memory
  that other code can reach.

## Optimizations to try

Each is checked against the invariants, then measured against the previous
library version and the paper version:

- SIMD for the bottom-up neighbor test over `dist8` and for gathering a
  block's undiscovered vertices (reads only; invariant 3 holds per byte and,
  for aligned four-byte distances, per element).
- Software prefetching of neighbor lists and of the `dist8` lines of
  upcoming neighbors in top-down expansion.
- NUMA placement of the per-vertex arrays by block ownership, and huge pages.
- A faster random generator than `mt19937` for the helpers' orders, now that
  no instruction-level equivalence ties the library to the paper's code.
- Narrower offsets when the graph has fewer than 2^32 edges.
- A record packed into 128 bytes rather than 256, which halves the memory of
  each level, at the cost of the separate lines that keep the CAS on its slot
  off the lines that readers of the plan need.

## Memory model

The paper's code orders its shared accesses with release stores and acquire
loads (relaxed elsewhere), and the first version keeps them. On x86-64 they
compile to plain moves that x86-TSO (stores leave in order, loads happen in
order) keeps in order, which is the model the proofs assume. C++ promises less
than TSO on other targets, such as ARM: a release store need not become
visible to all other threads at once. So the first version still refuses to
compile on targets other than x86-64, until the proofs are checked against
what C++ promises.

## Tests

- **Units:** affine orders are permutations, arena blocks are aligned and
  never overlap, and a bucket publishes an entry before its size.
- **Correctness:** paths, stars, grids, complete graphs, random graphs,
  disconnected graphs, an isolated source, and duplicate edges and self-loops,
  against the sequential BFS, at thread counts 1 to 64, with the default
  tuning constants and with small ones, so that small graphs reach hub chunks
  and many bottom-up blocks per thread. Long paths and graphs with thousands
  of levels check the reuse of the two nodes over many levels.
- **Robustness, without hooks:** the test harness stops worker threads from
  outside, by signalling their thread ids, and busy-waits in the signal
  handler, as `artifact/experiments/stalls/stall.hpp` does for its random
  stalls; the library needs nothing for this beyond `bfs_pool::thread_ids()`.
  Results must stay exact, and the first call to return must not wait for a
  stalled thread. Stalls at chosen points (right after a CAS, inside a hub
  chunk) need code hooks, so they remain tests of the paper version.

## Performance checks

- `wfgt/bench/` times a search as the paper's harness does (20 searches, the
  reset included, the first return as the end), so its numbers compare with
  the paper version's.
- Before a change is kept: exact distances, the robustness tests, and on the
  five standard graphs at 16, 64 and 112 threads, no point slower than the
  paper's WF-DO-mem, nor than the previous library version beyond noise.
  Where the paper's WF-DO is faster, by up to 5% on LiveJournal from 64 to
  112 threads, its times are the target.

## Steps

1. Set up `artifact/` from the current state, with every script, copy and
   data file behind the paper tracked, and an empty `wfgt/`.
2. Run the pending experiments from `artifact/`, then tag and archive it
   (`paper-v1`).
3. In parallel, since it touches nothing in `artifact/`: the detail headers,
   WF-DO-mem's levels among them, then `bfs` and `bfs_pool`, with the
   correctness and robustness tests and the timing driver.
4. Optimizations, one at a time, each against the invariants and the
   performance checks.

## Open questions

- Whether vertex and offset types should be template parameters. That costs
  nothing; int32 ids are enough for every graph in the paper.
- Directed graphs need a transposed CSR for bottom-up levels. The first
  version requires symmetric graphs, as the paper's code does.
- Whether to offer parents as well as distances.
- A license, given GAPBS (BSD) and GBBS (MIT) in the baselines.

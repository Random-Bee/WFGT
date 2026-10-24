# Fast Wait-Free Parallel Breadth-First Search: the paper's code and data

This directory holds the paper, in `paper/`, and what its tables and figures
come from: the programs of the main comparison, a file for each configuration
of the other experiments, the scripts, the upstream code of the baselines at
the commits used, and the data files. Our sources have no switches that choose
code: each file is one complete program, so each configuration can be read
directly, and compared with the main program by a diff. The
only values left to the build are the code shift of `benchmark/text-shift.h`
and the chunk size of `work/wf-dir-count.cpp` and `chunks/wf-dir-chunks.cpp`,
which the scripts set to 256; the code taken from GBBS keeps its
own configuration macros. The library implementation, WFGT, lives in
`../wfgt` and is not the code the paper measured (see `../docs/wfgt-design.md`).

From the tag `paper-v1` on, this directory takes only build fixes and
documented bug fixes.

## Contents

| Directory | What it holds |
|---|---|
| `WF_BFS/` | `wf-bfs.cpp`, the method of Section 3, with top-down levels only, to which WF-DO adds bottom-up levels; no table or figure uses it, and `benchmark.sh` runs it as `wfbfs` when `PROGRAMS` names it |
| `WF_DO_BFS/` | `wf-dir.cpp` (WF-DO) and `wf-dir-mem.cpp` (WF-DO-mem) |
| `DO_BFS/` | `dir-bfs.cpp`, GAPBS's direction-optimizing BFS taken out of GAPBS, and `dir-bfs-seq.cpp`, the sequential baseline |
| `GBBS_BFS/`, `PASGAL_BFS/` | drivers around GBBS's and PASGAL's BFS kernels |
| `submodules/` | upstream code at the commits used, as git submodules: `gapbs/`, `gbbs/` and `PASGAL/`, and `parlaylib/`, the ParlayLib that the GBBS builds use, which gbbs does not track at its commit |
| `cpu_helpers/`, `graph_helpers/` | thread pinning; the graph reader and the graph generators |
| `benchmark/` | `benchmark.sh` (the main comparison), `make-refs.sh` (the reference distances), `config.sh` (machine settings), `text-shift.h` (code shifts) |
| `experiments/` | a folder for each of the other experiments, holding its scripts and a file for each configuration it builds (below); at the top, `paper-data.sh` and `paper-stalls.sh`, which write the paper's data files, and `review-runs.sh` |
| `results/` | the data files behind the paper's tables and figures |
| `paper/` | the paper's LaTeX source; `latexmk -pdf BFS.tex` there builds it |

| `experiments/` | Experiments |
|---|---|
| `stalls/` | one thread stalled per level, and random stalls (Figures 6 to 8, Table 10); `stall.hpp` and the five stall builds, which the other folders reuse |
| `phases/` | the reset and the search of a run (Table 5), and 128 threads (Section 8.2) |
| `work/` | the work of each program (Table 6), and a direction rule that counts each vertex once (Section 8.3); also marks with fences, `wf-dir-fenced.cpp` (`wdfenced` in `benchmark.sh`, data in `marks*.csv`; not in the paper) |
| `chunks/` | affine and uniform orders (Section 8.3), and whole lists against chunks on the graph of hubs (Section 8.6); also the times of the two orders (the `orders-times` step of `review-runs.sh`, `orders-times.csv`; not in the paper) |
| `roots/` | other roots (Table 7) |
| `caches/` | caches on Road-USA (Section 8.5) |
| `memory/` | memory (Table 9) |
| `helping/` | the work that helpers do (Table 12) |
| `speedup/` | where the time of GAPBS and of WF-DO goes, and the gap between them split into a traversal term and bookkeeping (Table 11) |
| `copy/` | what copying a growing bucket's array with atomic loads costs, against the plain copy (not in the paper) |
| `ordering/` | what release stores and acquire loads cost, against the relaxed accesses and compiler barriers (not in the paper) |
| `wait-policy/` | GAPBS's wait policy (Section 8.1) |

## Environment of the paper's runs

- Two Intel Xeon Platinum 8592V sockets, 64 cores each, one NUMA node per
  socket (node 0 holds the even-numbered CPUs), otherwise idle while the
  experiments ran.
- GCC 15.2.1 (gcc-toolset-15), C++23, `-O3 -march=native -DNDEBUG -g
  -fno-omit-frame-pointer -falign-loops=32 -falign-jumps=32`, every program
  built five times with its code moved by 0 to 256 bytes (`text-shift.h`).
- GAPBS with the system libgomp and `GOMP_SPINCOUNT=300000`; GBBS and PASGAL
  with ParlayLib's defaults.
- `numactl`, `flock`, and for `experiments/caches/road-mem-perf.sh`, `perf`.

## Setup

1. Fetch the upstream code at its commits:
   `git submodule update --init --recursive`. This puts in `submodules/`
   gapbs `2972aeb`, gbbs `bd48872` and PASGAL `f7817ba`, with PASGAL's
   ParlayLib `c655b8f`, and the ParlayLib that the GBBS builds use,
   `352f536`, which gbbs does not track at its commit.
2. Settings come from `benchmark/config.sh`: the environment first, then
   `benchmark/local.sh` (which git ignores, for each machine's own), then the
   defaults. They are `BFS_CXX` (the compiler), `BFS_DATASETS` (the downloaded
   graphs), `BFS_REFS` (reference distances), `BFS_LOCK` (the lock that keeps
   two runs off the machine at once), `GAPBS_SPINCOUNT` and `BFS_PERF`.
3. Put the graphs in place (below), then write each graph's reference
   distances with the sequential baseline:
   `benchmark/make-refs.sh lj twitter road uk kron hubs chain`.

## Graphs

Downloaded, into `BFS_DATASETS`:

| Name in the scripts | File | Source |
|---|---|---|
| `lj` | `liveJournal1.edges` | LiveJournal, SNAP (http://snap.stanford.edu/data) |
| `twitter` | `twitter-2010.txt` | Twitter, Kwak et al., WWW 2010 |
| `road` | `road-road-usa.mtx` | Road-USA, Network Repository (https://networkrepository.com) |
| `uk` | `uk-2002.mtx` | uk-2002, WebGraph, from the SuiteSparse Matrix Collection |

Generated:

| Name in the scripts | File | Command |
|---|---|---|
| `kron` | `kron-25.edges`, in `BFS_DATASETS` | `graph_helpers/gapbs_generate.cpp` (build line in its header), then `./gapbs_generate -g 25 -e kron-25.edges` |
| `hubs` | `datasets/hubs.edges` | `graph_helpers/hubs_generate.cpp`, then `./hubs_generate 64 8 400000 1 datasets/hubs.edges` |
| `chain` | `datasets/chain.edges` | `awk 'BEGIN { n = 10000; print n, n - 1; for (i = 0; i < n - 1; i++) print i, i + 1 }' > datasets/chain.edges` |

The SHA-256 of each file used is listed at the end of this file.

## Running

- `benchmark/benchmark.sh [placements] [graphs]`: the main benchmark. Its
  header lists the options (`PROGRAMS`, `SHIFTS`, `COUNTS`, `RESULTS`,
  `DRY_RUN` and others); it writes to `benchmark/results/`.
- `experiments/stalls/stalls.sh` and `experiments/stalls/noise.sh`: one
  thread stalled per level, and every thread stalled at random;
  `GRAPH=twitter` for Twitter.
- `experiments/*/*.sh`: each script's header says what it measures, how to
  start it, and where its rows go, which is next to the script.
- Every script takes the lock, so runs queue up rather than overlap, and
  checks every run's distances against the reference: a program compares the
  distances after the 20 searches of each thread count with those of its first
  thread count, printing `differs` if they are not the same, and writes those
  of its last thread count, which the script compares with the sequential
  baseline's.
- The paper's data files, from `results/` by default:
  `experiments/paper-data.sh > paper/sections/data.tex` and
  `experiments/paper-stalls.sh > paper/sections/data-stalls.tex`.

## Tables and figures

"Benchmark builds" are those of `benchmark/benchmark.sh`: WF-DO
(`WF_DO_BFS/wf-dir.cpp`), WF-DO-mem (`WF_DO_BFS/wf-dir-mem.cpp`), GAPBS
(`DO_BFS/dir-bfs.cpp`), GBBS, PASGAL and the sequential baseline
(`DO_BFS/dir-bfs-seq.cpp`).

| In the paper | Script and options | Builds | Data in `results/` |
|---|---|---|---|
| Table 3, graphs | counts from the graph files; levels from the reference distances (`benchmark/make-refs.sh`) | sequential baseline | |
| Table 4, Figure 5, Tables 14 to 19, times per thread count | `RESULTS=final-single SKIP="both,road,gapbs,128" benchmark/benchmark.sh both`, then GAPBS on Road-USA at 128 threads on its own, `RESULTS=final-single PROGRAMS=gapbs COUNTS=128 benchmark/benchmark.sh both road`; `RESULTS=hubs-single benchmark/benchmark.sh both hubs`; then `experiments/paper-data.sh` | Benchmark builds, five shifts each | `final-single.csv`, `hubs-single.csv` and their `-summary.csv` |
| Table 5, reset and search | `experiments/phases/diag-phases.sh` | `stalls/wf-dir-stalls.cpp`, `stalls/wf-dir-mem-stalls.cpp`, no stalls set | `diag-phases.csv` |
| Section 8.2, 128 threads | `experiments/phases/diag128.sh`, and with `GRAPH` set to each of `twitter`, `road`, `uk`, `kron` and `hubs` | the five stall builds, `stalls/*-stalls.cpp`, no stalls set | `diag128.csv`, `diag128-<graph>.csv` |
| Section 8.2, the setup of the other programs | later runs of `experiments/phases/diag128.sh`, with the builds of GAPBS, GBBS and PASGAL that time their setup apart from their traversal (in the columns of our reset and search) | as above | `diag128-setup.csv`, `diag128-<graph>-setup.csv` |
| Table 6, work | `experiments/work/work-counts.sh` | `work/wf-dir-count.cpp`, `work/wf-dir-count-stalls.cpp`, `work/dir-bfs-count.cpp`, `work/dir-bfs-seq-count.cpp` | `work-counts.csv` |
| Section 8.3, a direction rule that counts each vertex once | `STEPS="exact-counts exact-times" experiments/review-runs.sh` | `work/wf-dir-count-exact.cpp`; `work/wf-dir-exact.cpp`, `wdexact` in `benchmark.sh` | `work-counts-exact.csv`, `exact.csv`, `exact-summary.csv` |
| Section 8.3, affine and uniform orders | `STEPS=orders-counts experiments/review-runs.sh` | `work/wf-dir-count.cpp`, `chunks/wf-dir-count-uniform.cpp` | `orders-counts.csv`, `orders-counts-levels.csv` |
| Table 7, other roots | `experiments/roots/roots.sh` | Benchmark builds, unshifted | `roots-both.csv`, `roots-both-summary.csv` |
| Table 8, a path | `RESULTS=chain COUNTS="1 2 4 8 16 24 32 40 48 56 64 80 96 112" benchmark/benchmark.sh both chain` | Benchmark builds | `chain.csv` |
| Section 8.5, caches on Road-USA | `experiments/caches/road-mem-perf.sh` | the benchmark's builds of WF-DO and WF-DO-mem, shift 0 | `road-mem-perf.csv` and `road-mem-perf-earlier.csv`, two runs of the script, whose counts the paper adds up |
| Section 8.6, whole lists on the graph of hubs | `STEPS="whole-counts whole-times" experiments/review-runs.sh` | `chunks/wf-dir-count-whole.cpp`, `chunks/wf-dir-whole.cpp`, against `work/wf-dir-count.cpp` and `chunks/wf-dir-chunks.cpp` with chunks of 256 | `whole-counts.csv`, `whole-counts-levels.csv`, `whole-times.csv` |
| Table 9, memory | `experiments/memory/memory.sh` | `memory/wf-dir-memory.cpp`, `memory/wf-dir-mem-memory.cpp` | `memory-both.csv` |
| Figure 6 and Tables 20 to 25, one thread stalled per level | `experiments/stalls/stalls.sh` without `GRAPH` (LiveJournal) and with `GRAPH` set to each of `twitter`, `road`, `uk`, `kron` and `hubs`; then `experiments/paper-stalls.sh`; the confidence intervals of Section 8.8, `python3 experiments/stalls/stall-ci.py results/stalls-twitter.csv` | the five stall builds, `stalls/*-stalls.cpp` | `stalls-twitter*.csv`, `stalls-road*.csv`, `stalls-both*.csv` (LiveJournal), `stalls-<graph>*.csv` |
| Figures 7 and 8, Tables 26 to 37 and Table 10, random stalls and stall points | `experiments/stalls/noise.sh` with the graphs as above; `experiments/stalls/diag-noise.sh` for the reset and the search | as above | `noise-twitter*.csv`, `noise-road*.csv`, `noise-both*.csv` (LiveJournal), `noise-<graph>*.csv`, `diag-noise*.csv` |
| Table 11, accounting for the performance gap | `experiments/speedup/steps.sh`, then `experiments/speedup/summarize.py` | `speedup/dir-bfs-steps.cpp` and `speedup/wf-dir-steps.cpp`, with the benchmark builds of GAPBS and WF-DO, shift 0, alongside | `speedup-steps.csv`, the step reports in `speedup-logs/`, and the splits in `speedup-summary.csv` (`summarize.py --logs results/speedup-logs --csv results/speedup-steps.csv --out results/speedup-summary.csv` remakes them) |
| Table 12, the work of helpers | `experiments/helping/help-stalls.sh` | `work/wf-dir-count-stalls.cpp` | `help-stalls-both.csv`, `help-stalls-both-summary.csv` |

## SHA-256 of the graphs

```text
7143bc7ddff32f069c5b1fbdff590ab6083870b03a9d36387d5ba694b2fc08cc  liveJournal1.edges
1ff972a37c785e89d3e7204875534c060932e62a3667347928019afbc22b00ec  twitter-2010.txt
5ee32b5ac21e95c8c4a9bf14aa3725215cc8f4104df88284b53913f43252b990  road-road-usa.mtx
55ebc64f04efdac7e4111a0a93628584f177e5630ec8047dd16c3d288590b1a4  uk-2002.mtx
841310b247b30f14d3e5f54da12b0a2d7d588ea6b783509bb7f7e264a990a3e9  kron-25.edges
38a2c17cc520cdb060024ad61985ab07d7c54b26a0f00e0125d7dd56ef2fa361  hubs.edges
9343b26119769819d5bb810e0021f8a3f07a03044a3d6bcc9aeb765cf29d9efa  chain.edges
```

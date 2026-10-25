# WFGT: Wait-Free Parallel Breadth-First Search (BFS)

This repository holds the paper *Fast Wait-Free Parallel Breadth-First Search*
(Aayush Kumar, 2026), its implementation, and the scripts and data behind
every table and figure in it.

**[Read the research manuscript (PDF)](artifact/paper/BFS.pdf)**

The paper presents a parallel BFS for shared memory in which threads share the
work of every level without barriers. Each call returns after a finite number
of its own steps, however slow the other threads are, so a thread that is
preempted or stalled never holds up the others. The method is proved correct
and wait-free under the memory model of x86, given that allocation takes a
bounded number of steps.

## The paper's implementation

[`artifact/`](artifact/) holds the paper's two methods, WF-DO and WF-DO-mem,
the baselines at the commits used, a file for each configuration of the other
experiments, the scripts, the data files and the paper's LaTeX source. Its
[README](artifact/README.md) maps every table and figure to the script, build
and data file behind it. From the tag `paper-v1` on, it takes only build fixes
and documented bug fixes.

## The WFGT library

WFGT (wait-free graph traversal) will be a C++ library implementing WF-DO-mem,
the variant of the paper's method whose memory depends on the largest levels
of a search rather than on all of them. It keeps the protocol that the paper
proves correct and wait-free, but none of the paper's code.

**Status:** early development. `wfgt/` holds only its layout so far. The first
version will compute exact distances on symmetric graphs in CSR form, on
x86-64, from threads the caller owns or from a pinned thread pool. The plan,
the API sketch and the invariants the code must keep are in
[`docs/wfgt-design.md`](docs/wfgt-design.md).

## Repository

- `artifact/`: the paper's code, scripts, data and LaTeX source.
- `wfgt/`: the library (`include/wfgt/`), its tests (`tests/`) and its timing
  driver (`bench/`).
- `docs/`: the library's design note.

## Citation

Aayush Kumar. *Fast Wait-Free Parallel Breadth-First Search*. 2026.

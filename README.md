# WFGT: wait-free graph traversal

WFGT will be a C++ library for wait-free parallel breadth-first search on
shared memory. It implements WF-DO-mem, the variant of the method of the paper
*Fast Wait-Free Parallel Breadth-First Search* whose memory depends on the
largest levels of a search rather than on all of them. Threads share the work
of every level without barriers, and each call returns after a finite number of
its own steps, however slow the other threads are, so a thread that is
preempted or stalled never holds up the others. The library keeps the protocol
that the paper proves correct and wait-free, but none of the paper's code,
which lives in `artifact/`.

**Status:** early development. `wfgt/` holds only its layout so far. The first
version will compute exact distances on symmetric graphs in CSR form, on
x86-64, from threads the caller owns or from a pinned thread pool. The plan,
the API sketch and the invariants the code must keep are in
[`docs/wfgt-design.md`](docs/wfgt-design.md).

## Repository

- `wfgt/`: the library (`include/wfgt/`), its tests (`tests/`) and its timing
  driver (`bench/`).
- `artifact/`: the paper's code, scripts, data and LaTeX source; from the tag
  `paper-v1` on, it takes only build fixes and documented bug fixes. Its
  [README](artifact/README.md) maps every table
  and figure to the script, build and data file behind it.
- `docs/`: the library's design note.

## Paper

Aayush Kumar. *Fast Wait-Free Parallel Breadth-First Search*. 2026.

[Read the research manuscript (PDF)](artifact/paper/BFS.pdf).

// PASGAL, the Parallel And Scalable Graph Algorithm Library
// (github.com/ucrparlay/PASGAL, commit f7817ba)
//
// MIT License
//
// Copyright (c) 2025 UCR Parallel Algorithm Lab (UCR-PAL)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <dirent.h>
#include <sched.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "parlay/delayed_sequence.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "parlay/slice.h"
#include "parlay/utilities.h"

#include "../cpu_helpers/cpu_affinity.hpp"
#include "../graph_helpers/graph_reader.hpp"


// The parts of PASGAL's headers (utils.h, sampler.h, hashbag.h, graph.h) that
// the kernel uses

template <typename ET>
inline bool atomic_compare_and_swap(ET *a, ET oldval, ET newval) {
  static_assert(sizeof(ET) <= 8, "Bad CAS length");
  if constexpr (sizeof(ET) == 1) {
    uint8_t r_oval, r_nval;
    std::memcpy(&r_oval, &oldval, sizeof(ET));
    std::memcpy(&r_nval, &newval, sizeof(ET));
    return __sync_bool_compare_and_swap(reinterpret_cast<uint8_t *>(a), r_oval,
                                        r_nval);
  } else if constexpr (sizeof(ET) == 4) {
    uint32_t r_oval, r_nval;
    std::memcpy(&r_oval, &oldval, sizeof(ET));
    std::memcpy(&r_nval, &newval, sizeof(ET));
    return __sync_bool_compare_and_swap(reinterpret_cast<uint32_t *>(a), r_oval,
                                        r_nval);
  } else if constexpr (sizeof(ET) == 8) {
    uint64_t r_oval, r_nval;
    std::memcpy(&r_oval, &oldval, sizeof(ET));
    std::memcpy(&r_nval, &newval, sizeof(ET));
    return __sync_bool_compare_and_swap(reinterpret_cast<uint64_t *>(a), r_oval,
                                        r_nval);
  } else {
    std::cerr << "Bad CAS length" << std::endl;
  }
}

template <class ET>
inline bool compare_and_swap(std::atomic<ET> *a, ET oldval, ET newval) {
  return a->load(std::memory_order_relaxed) == oldval &&
         atomic_compare_exchange_weak(a, &oldval, newval);
}

template <class ET>
inline bool compare_and_swap(ET *a, ET oldval, ET newval) {
  return (*a) == oldval && atomic_compare_and_swap(a, oldval, newval);
}

template <typename ET, typename F = std::less<ET>>
inline bool write_min(ET *a, ET b, F less = {}) {
  ET c;
  bool r = 0;
  do c = *a;
  while (less(b, c) && !(r = atomic_compare_and_swap(a, c, b)));
  return r;
}

using hash_t = uint32_t;

class Sampler {
  std::atomic<uint32_t> num_hits;
  uint32_t exp_hits;
  hash_t threshold;

 public:
  Sampler() {}

  Sampler(const size_t _exp_hits, double _sample_rate)
      : exp_hits(_exp_hits),
        threshold(_sample_rate * std::numeric_limits<hash_t>::max()) {
    num_hits = 0;
  }

  Sampler(const Sampler &other)
      : exp_hits(other.exp_hits), threshold(other.threshold) {
    num_hits = other.num_hits.load();
  }

  Sampler(Sampler &&other)
      : exp_hits(other.exp_hits), threshold(other.threshold) {
    num_hits = other.num_hits.load();
  }

  bool sample(hash_t random_number, bool &callback) {
    callback = false;
    if (num_hits >= exp_hits) {
      return false;
    }
    if (random_number < threshold) {
      uint32_t ret = num_hits.fetch_add(1);
      if (ret >= exp_hits) {
        return false;
      } else if (ret + 1 == exp_hits) {
        callback = true;
      }
    }
    return true;
  }

  void reset() { num_hits = 0; }
};

template <class ET>
class hashbag {
  static constexpr size_t BLOCK_SIZE = 1 << 10;
  static constexpr size_t MIN_BAG_SIZE = 1 << 6;
  static constexpr size_t MAX_PROBES = 1000;
  static constexpr size_t EXP_NUM_SAMPLES = 32;

  size_t n;
  const ET empty;
  std::atomic<uint32_t> bag_id;

  parlay::sequence<size_t> bag_sizes;
  parlay::sequence<size_t> offsets;
  parlay::sequence<Sampler> samplers;
  parlay::sequence<ET> pool;

 public:
  hashbag() = default;

  hashbag(size_t _n, double load_factor = 0.5,
          const ET _empty = std::numeric_limits<ET>::max())
      : n(_n), empty(_empty) {
    bag_id = 0;
    size_t cur_size = MIN_BAG_SIZE;
    size_t total_size = 0;
    while (total_size * load_factor < n) {
      double sample_rate = EXP_NUM_SAMPLES / (cur_size * load_factor);
      bag_sizes.push_back(cur_size);
      offsets.push_back(total_size);
      samplers.push_back(Sampler(EXP_NUM_SAMPLES, sample_rate));
      total_size += cur_size;
      cur_size *= 2;
    }
    pool = parlay::sequence<ET>(total_size, empty);
  }

  hashbag(const hashbag &other)
      : n(other.n),
        empty(other.empty),
        bag_id(other.bag_id.load()),
        bag_sizes(other.bag_sizes),
        offsets(other.offsets),
        samplers(other.samplers),
        pool(other.pool) {}

  hashbag(hashbag &&other)
      : n(other.n),
        empty(other.empty),
        bag_id(other.bag_id.load()),
        bag_sizes(other.bag_sizes),
        offsets(other.offsets),
        samplers(other.samplers),
        pool(other.pool) {}

  void clear() {
    for (size_t i = 0; i <= bag_id; i++) {
      samplers[i].reset();
    }
    parlay::parallel_for(
        0, offsets[bag_id] + bag_sizes[bag_id],
        [&](size_t i) { pool[i] = empty; }, BLOCK_SIZE);
    bag_id = 0;
  }

  void insert(ET u) {
    uint32_t local_id = bag_id;
    auto random_number = parlay::hash32(u);
    size_t idx = random_number & (bag_sizes[local_id] - 1);
    bool callback = false;
    while (local_id + 1 < bag_sizes.size() &&
           !samplers[local_id].sample(random_number, callback)) {
      compare_and_swap(&bag_id, local_id, local_id + 1);
      local_id = bag_id;
    }
    size_t num_probes = 0;
    idx = random_number & (bag_sizes[local_id] - 1);
    while (!compare_and_swap(&pool[offsets[local_id] + idx], empty, u)) {
      idx++;
      if (idx == bag_sizes[local_id]) {
        idx = 0;
      }
      num_probes++;
      if (num_probes == bag_sizes[local_id] || num_probes == MAX_PROBES) {
        num_probes = 0;
        compare_and_swap(&bag_id, local_id, local_id + 1);
        if (local_id != bag_id) {
          local_id = bag_id;
          assert(local_id < bag_sizes.size() && "hashbag is full");
          idx = random_number & (bag_sizes[local_id] - 1);
        }
      }
    }
  }

  template <typename Seq>
  size_t pack_into(Seq &&out) {
    size_t len = offsets[bag_id] + bag_sizes[bag_id];
    auto pred = parlay::delayed_seq<bool>(
        len, [&](size_t i) { return pool[i] != empty; });
    size_t num_records = parlay::pack_into_uninitialized(pool.cut(0, len), pred,
                                                         make_slice(out));
    clear();
    return num_records;
  }
};

class Empty {};

template <class NodeId, class EdgeTy>
class WEdge {
 public:
  NodeId v;
  [[no_unique_address]] EdgeTy w;
  WEdge() {}
  WEdge(NodeId _v) : v(_v) {}
  WEdge(NodeId _v, EdgeTy _w) : v(_v), w(_w) {}
};

template <class _NodeId = uint32_t, class _EdgeId = uint64_t,
          class _EdgeTy = Empty>
class Graph {
 public:
  using NodeId = _NodeId;
  using EdgeId = _EdgeId;
  using EdgeTy = _EdgeTy;
  using Edge = WEdge<NodeId, EdgeTy>;

  size_t n;
  size_t m;
  bool symmetrized;
  bool weighted;
  parlay::sequence<EdgeId> offsets;
  parlay::sequence<Edge> edges;
  parlay::sequence<EdgeId> in_offsets;
  parlay::sequence<Edge> in_edges;

  Graph() {
    n = m = 0;
    symmetrized = weighted = false;
  }

  auto in_neighors(NodeId u) const {
    if (symmetrized) {
      return edges.cut(offsets[u], offsets[u + 1]);
    } else {
      return in_edges.cut(in_offsets[u], in_offsets[u + 1]);
    }
  }
};


// PASGAL's BFS kernel (src/BFS/bfs.h). bfs() returns its distances where they
// are, rather than a copy of them, as GAPBS's are handed over

using namespace std;
using namespace parlay;

template <class Graph>
class BFS {
  using NodeId = typename Graph::NodeId;
  using EdgeId = typename Graph::EdgeId;

  static constexpr NodeId DIST_MAX = numeric_limits<NodeId>::max();
  static constexpr size_t LOCAL_QUEUE_SIZE = 128;
  static constexpr size_t BLOCK_SIZE = 1024;
  static constexpr size_t NUM_SAMPLES = 1024;
  static constexpr size_t SPARSE_TH = 20;
  static constexpr size_t GROWTH_FACTOR = 10;

  const Graph &G;
  const int LOG2N;
  const size_t num_bags;
  size_t round;
  sequence<hashbag<NodeId>> bags;
  sequence<NodeId> frontier;
  sequence<NodeId> dist;
  sequence<int> bag_id;
  sequence<atomic<bool>> in_frontier;
  bool sparse;
  bool use_local_queue;

 public:
  BFS() = delete;
  BFS(const Graph &_G)
      : G(_G), LOG2N(log2_up(G.n)), num_bags(log2_up(LOCAL_QUEUE_SIZE) + 2) {
    bags = sequence<hashbag<NodeId>>(num_bags, hashbag<NodeId>(G.n));
    frontier = sequence<NodeId>::uninitialized(G.n);
    dist = sequence<NodeId>::uninitialized(G.n);
    bag_id = sequence<int>::uninitialized(G.n);
    in_frontier = sequence<atomic<bool>>(G.n);
  }

  void add_to_frontier(NodeId v) {
    if (sparse) {
      int id = dist[v] == 0 ? 0 : log2_up(dist[v]);
      if (in_frontier[v] == false) {
        in_frontier[v] = true;
        write_min(&bag_id[v], id);
        bags[id % num_bags].insert(v);
      } else {
        if (write_min(&bag_id[v], id)) {
          bags[id % num_bags].insert(v);
        }
      }
    } else {
      in_frontier[v] = true;
    }
  }

  size_t estimate_size([[maybe_unused]] size_t id) {
    static uint32_t seed = 951;
    size_t hits = 0;
    for (size_t i = 0; i < NUM_SAMPLES; i++) {
      NodeId u = hash32(seed) % G.n;
      if (dist[u] == round) {
        hits++;
      }
      seed++;
    }
    return hits * G.n / NUM_SAMPLES;
  }

  void visit_neighbors_parallel(NodeId u) {
    parallel_for(
        G.offsets[u], G.offsets[u + 1],
        [&](size_t i) {
          NodeId v = G.edges[i].v;
          if (write_min(&dist[v], dist[u] + 1)) {
            add_to_frontier(v);
          }
        },
        BLOCK_SIZE);
  }

  void visit_neighbors_sequential(NodeId u, NodeId *local_queue, size_t &rear) {
    for (EdgeId i = G.offsets[u]; i < G.offsets[u + 1]; i++) {
      NodeId v = G.edges[i].v;
      if (write_min(&dist[v], dist[u] + 1)) {
        if (rear < LOCAL_QUEUE_SIZE) {
          local_queue[rear++] = v;
        } else {
          add_to_frontier(v);
        }
      }
    }
  }

  void dense2sparse() {
    for (size_t i = 0; i < num_bags; i++) {
      bags[i].clear();
    }
    parallel_for(0, G.n, [&](size_t i) {
      if (in_frontier[i]) {
        size_t id = log2_up(dist[i]);
        bags[id % num_bags].insert(i);
      }
    });
  }

  void sparse_relax(size_t id, size_t frontier_size) {
    parallel_for(0, frontier_size, [&](size_t i) {
      NodeId f = frontier[i];
      in_frontier[f] = false;
      if (id == 0 || id == log2_up(dist[f])) {
        if (use_local_queue) {
          NodeId local_queue[LOCAL_QUEUE_SIZE];
          size_t front = 0, rear = 0;
          local_queue[rear++] = f;
          while (front < rear) {
            NodeId u = local_queue[front++];
            size_t deg = G.offsets[u + 1] - G.offsets[u];
            if (deg < BLOCK_SIZE) {
              visit_neighbors_sequential(u, local_queue, rear);
            } else {
              visit_neighbors_parallel(u);
            }
          }
        } else {
          visit_neighbors_parallel(f);
        }
      }
    });
  }

  void dense_relax([[maybe_unused]] size_t id) {
    parallel_for(0, G.n, [&](NodeId u) {
      if (dist[u] > round + 1) {
        const auto neighbors = G.in_neighors(u);
        for (size_t j = 0; j < neighbors.size(); j++) {
          NodeId v = neighbors[j].v;
          if (dist[v] != DIST_MAX && dist[u] > dist[v] + 1) {
            dist[u] = dist[v] + 1;
            in_frontier[u].store(true, std::memory_order_relaxed);
            if (dist[v] == round) {
              break;
            }
          }
        }
      } else if (dist[u] <= round) {
        in_frontier[u].store(false, std::memory_order_relaxed);
      }
    });
  }

  bool if_sparse(size_t frontier_size) {
    return frontier_size * SPARSE_TH < G.n;
  }

  const sequence<NodeId> &bfs(NodeId s) {
    parallel_for(0, G.n, [&](size_t i) {
      in_frontier[i] = false;
      dist[i] = DIST_MAX;
      bag_id[i] = LOG2N;
    });

    sparse = true;
    dist[s] = 0;
    add_to_frontier(s);

    round = 0;
    size_t prev_size = 0;
    bool dense = false;
    for (int i = 0; i <= LOG2N; i++) {
      if (i != 0) {
        round = max(round, (size_t)((1 << (i - 1)) + 1));
      }
      while (true) {
        size_t approx_size = estimate_size(i);
        if (if_sparse(approx_size)) {
          if (dense) {
            dense2sparse();
          }
          size_t frontier_size =
              bags[i % num_bags].pack_into(make_slice(frontier));
          if (frontier_size <= prev_size * GROWTH_FACTOR) {
            use_local_queue = true;
          } else {
            use_local_queue = false;
          }
          prev_size = frontier_size;
          if (!frontier_size) {
            break;
          }
          sparse_relax(i, frontier_size);
          dense = false;
          round++;
        } else {
          dense_relax(i);
          dense = true;
          round++;
          prev_size = approx_size;
        }
      }
    }

    for (size_t i = 0; i < num_bags; i++) {
      assert(bags[i].pack_into(make_slice(frontier)) == 0);
    }
    return dist;
  }
};


// Driver: kept the same as wf-bfs.cpp's, so the two are timed and reported
// the same way

using pasgal_graph = Graph<uint32_t, uint64_t, Empty>;

int N;
size_t M;

int num_t;

[[gnu::noinline]] void control_perf(const char* command) {
    static FILE* control = nullptr;
    static FILE* ack = nullptr;
    static bool initialized = false;

    if(!initialized) {
        const char* control_path = getenv("PERF_CTL_FIFO");
        const char* ack_path = getenv("PERF_ACK_FIFO");

        if(control_path == nullptr && ack_path == nullptr) {
            initialized = true;
            return;
        }
        if(control_path == nullptr || ack_path == nullptr) {
            cerr << "Error: Both PERF_CTL_FIFO and PERF_ACK_FIFO "
                 << "must be set\n";
            exit(EXIT_FAILURE);
        }

        control = fopen(control_path, "w");
        if(control == nullptr) {
            cerr << "Error: Failed to open perf control FIFO\n";
            exit(EXIT_FAILURE);
        }

        ack = fopen(ack_path, "r");
        if(ack == nullptr) {
            cerr << "Error: Failed to open perf acknowledgement FIFO\n";
            exit(EXIT_FAILURE);
        }

        initialized = true;
    }

    if(control == nullptr) return;

    if(fprintf(control, "%s\n", command) < 0 || fflush(control) != 0) {
        cerr << "Error: Failed to send command to perf\n";
        exit(EXIT_FAILURE);
    }

    constexpr char expected[] = "ack\n";
    char response[sizeof(expected)];
    if(fread(response, 1, sizeof(response), ack) != sizeof(response) ||
            memcmp(response, expected, sizeof(expected)) != 0) {
        cerr << "Error: Invalid acknowledgement from perf\n";
        exit(EXIT_FAILURE);
    }
}

// the CSR the shared reader builds, copied into arrays PASGAL's graph owns, on
// huge pages as the reader's own arrays are. Neighbors keep file order and
// duplicates, as in wf-bfs. Every edge is listed at both its ends, so the
// graph is symmetrized, and bottom-up steps read the same lists
[[gnu::noinline]] std::unique_ptr<pasgal_graph> load_graph(const std::string& path) {
    graph_helpers::array<size_t> offsets;
    graph_helpers::array<int> adj;
    {
        graph_helpers::edge_list edges = graph_helpers::read_graph(path);
        N = edges.n;
        M = edges.from.size();
        graph_helpers::build_csr(edges, offsets, adj);
    }

    const size_t vertices = static_cast<size_t>(N);
    const size_t total = adj.size();
    std::unique_ptr<pasgal_graph> g(new pasgal_graph());
    g->n = vertices;
    g->m = total;
    g->symmetrized = true;
    g->offsets = parlay::sequence<uint64_t>::uninitialized(vertices + 1);
    g->edges = parlay::sequence<pasgal_graph::Edge>::uninitialized(total);
    graph_helpers::advise_huge_pages(
        g->offsets.begin(), sizeof(uint64_t) * (vertices + 1));
    graph_helpers::advise_huge_pages(
        g->edges.begin(), sizeof(pasgal_graph::Edge) * total);

    const int threads = graph_helpers::load_threads;
    graph_helpers::detail::run_on_threads(threads, [&](int t) {
        for(size_t i=total*t/threads; i<total*(t+1)/threads; i++) {
            g->edges[i].v = static_cast<uint32_t>(adj[i]);
        }
        for(size_t u=(vertices+1)*t/threads; u<(vertices+1)*(t+1)/threads; u++) {
            g->offsets[u] = offsets[u];
        }
    });
    return g;
}

// the ids of the process's threads, the main thread's first
[[gnu::noinline]] std::vector<pid_t> list_threads() {
    std::vector<pid_t> tids;
    DIR* tasks = opendir("/proc/self/task");
    if(tasks == nullptr) return tids;
    while(dirent* entry = readdir(tasks)) {
        char* end = nullptr;
        const long tid = strtol(entry->d_name, &end, 10);
        if(end != entry->d_name && *end == '\0' && tid > 0) {
            tids.push_back(static_cast<pid_t>(tid));
        }
    }
    closedir(tasks);
    std::sort(tids.begin(), tids.end());
    auto main_thread = std::find(tids.begin(), tids.end(), getpid());
    if(main_thread != tids.end()) {
        std::rotate(tids.begin(), main_thread, main_thread + 1);
    }
    return tids;
}

// pins thread tid, not necessarily the calling one, to cpu and names it
[[gnu::noinline]] bool pin_thread(pid_t tid, int cpu, const std::string& name) {
    if(cpu < 0 || cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if(sched_setaffinity(tid, sizeof(set), &set) != 0) return false;
    std::ofstream("/proc/self/task/" + std::to_string(tid) + "/comm") << name;
    return true;
}

// a ParlayLib scheduler starts all its workers when made, the calling thread
// being worker 0, and keeps them for every parallel loop it runs, so pinning
// them once holds for all repetitions. Its idle workers sleep until new work
// wakes one, and a wake-up can be missed, so parallel tasks are not sure to
// reach every worker; by now the process's threads are just the workers, so
// they are pinned by their thread ids instead
[[gnu::noinline]] bool start_threads(const std::vector<int>& cpu_ids) {
    const std::vector<pid_t> threads = list_threads();
    if(static_cast<int>(threads.size()) != num_t) return false;
    for(int i=0; i<num_t; i++) {
        if(!pin_thread(threads[i], cpu_ids[i], "pasgal-" + std::to_string(i))) {
            return false;
        }
    }
    return true;
}

[[gnu::noinline]] void write_output(const std::vector<int>& dist) {
    graph_helpers::write_distances("pasgal-out.txt", dist);
}

[[gnu::noinline]] int main(int argc, char *argv[]) {
    ios_base::sync_with_stdio(false); cin.tie(0); cout.tie(0);

    if(argc != 3) {
        cout << "Usage: " << argv[0] << " <input_file> <num_threads>[,<num_threads>...]\n";
        return 1;
    }

    std::vector<int> thread_counts;
    try {
        thread_counts = cpu_helpers::parse_thread_counts(argv[2]);
    }
    catch(const std::exception& error) {
        cerr << "Error: " << error.what() << "\n";
        return 1;
    }
    // ParlayLib starts a default scheduler when the graph's arrays are made,
    // reading this once; with one worker it starts no threads, and each thread
    // count gets a scheduler of its own
    if(setenv("PARLAY_NUM_THREADS", "1", 1) != 0) {
        cerr << "Error: Failed to set PARLAY_NUM_THREADS\n";
        return 1;
    }

    std::unique_ptr<pasgal_graph> g;
    uint32_t source = 0;
    try {
        g = load_graph(argv[1]);
        source = static_cast<uint32_t>(graph_helpers::search_source(g->n));
    }
    catch(const std::exception& error) {
        cerr << "Error: " << error.what() << "\n";
        return 1;
    }

    // given several thread counts, a line for each, its distances checked
    // against the first one's; the last one's are written
    std::vector<int> first_dist;
    std::vector<int> dist;
    long long average_duration = 0;
    for(size_t c = 0; c < thread_counts.size(); c++) {
        if(c > 0) std::this_thread::sleep_for(cpu_helpers::pause_between_thread_counts);
        num_t = thread_counts[c];

        std::vector<int> cpu_ids;
        try {
            cpu_ids = cpu_helpers::select_idle_cpus(num_t);
        }
        catch(const std::exception& error) {
            cerr << "Error: " << error.what() << "\n";
            return 1;
        }

        cerr << "Selected CPUs:";
        for(int cpu: cpu_ids) cerr << " " << cpu;
        cerr << "\n";

        bool ran = false;
        parlay::execute_with_scheduler(num_t, [&] {
            if(static_cast<int>(parlay::num_workers()) != num_t) {
                cerr << "Error: ParlayLib started " << parlay::num_workers()
                     << " workers rather than " << num_t << "\n";
                return;
            }
            if(!start_threads(cpu_ids)) {
                cerr << "Error: Failed to pin threads to the selected CPUs\n";
                return;
            }

            // set up once for the thread count, as PASGAL's own driver sets it
            // up once, so every search reuses its arrays
            BFS<pasgal_graph> solver(*g);

            constexpr int repetitions = 20;
            long long total_duration = 0;
            const parlay::sequence<uint32_t>* result = nullptr;

            control_perf("enable");

            for(int i=0; i<repetitions; i++) {
                auto t1 = std::chrono::high_resolution_clock::now();
                result = &solver.bfs(source);
                auto t2 = std::chrono::high_resolution_clock::now();

                total_duration += std::chrono::duration_cast<std::chrono::microseconds>(
                    t2 - t1).count();
            }

            control_perf("disable");

            average_duration = total_duration / repetitions;
            dist.assign(result->begin(), result->end());
            ran = true;
        });
        if(!ran) return 1;

        if(thread_counts.size() > 1) {
            if(c == 0) first_dist = dist;
            cout << num_t << " " << average_duration << " "
                 << (dist == first_dist ? "same" : "differs") << std::endl;
        }
    }

    write_output(dist);

    if(thread_counts.size() == 1) cout << average_duration << "\n";

    return 0;
}

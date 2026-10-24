// GBBS_BFS/gbbs-bfs.cpp with the stalls of stall.hpp, and the time of its
// traversal apart from the setup before it, for stalls.sh, noise.sh and
// diag128.sh
// This code is part of the project "Theoretically Efficient Parallel Graph
// Algorithms Can Be Fast and Scalable", presented at Symposium on Parallelism
// in Algorithms and Architectures, 2018 (github.com/ParAlg/gbbs, commit
// bd48872).
// Copyright (c) 2018 Laxman Dhulipala, Guy Blelloch, and Julian Shun
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all  copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <dirent.h>
#include <limits.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "parlay/delayed_sequence.h"
#include "parlay/internal/binary_search.h"
#include "parlay/internal/get_time.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "parlay/slice.h"
#include "parlay/utilities.h"

#include "../../cpu_helpers/cpu_affinity.hpp"
#include "../../graph_helpers/graph_reader.hpp"
#include "stall.hpp"


// The parts of GBBS's library (gbbs/bridge.h, macros.h, flags.h,
// vertex_subset.h, edge_map_utils.h, vertex.h, graph.h, edge_map_blocked.h,
// edge_map_data.h) that the kernel uses on a symmetric, unweighted,
// uncompressed graph

namespace gbbs {
// ================== parallel primitives ===================

using parlay::parallel_for;
using parlay::num_workers;

template <class E>
E* new_array_no_init(size_t n) {
#ifndef PARLAY_USE_STD_ALLOC
  auto allocator = parlay::allocator<E>();
#else
  auto allocator = std::allocator<E>();
#endif
  return allocator.allocate(n);
}

template <class E>
void free_array(E* e, size_t n) {
#ifndef PARLAY_USE_STD_ALLOC
  auto allocator = parlay::allocator<E>();
#else
  auto allocator = std::allocator<E>();
#endif
  allocator.deallocate(e, n);
}

// Alias template for parlay::sequence
template <typename T>
using sequence = parlay::sequence<T>;

struct empty {
};  // struct containing no data (used for empty base optimization)

// ========================= timer  ==========================
using parlay::internal::timer;

// ========================= atomic ops  ==========================

template <typename ET>
inline bool atomic_compare_and_swap(ET* a, ET oldval, ET newval) {
  if
    constexpr(sizeof(ET) == 1) {
      uint8_t r_oval, r_nval;
      std::memcpy(&r_oval, &oldval, sizeof(ET));
      std::memcpy(&r_nval, &newval, sizeof(ET));
      return __sync_bool_compare_and_swap(reinterpret_cast<uint8_t*>(a), r_oval,
                                          r_nval);
    }
  else if
    constexpr(sizeof(ET) == 4) {
      uint32_t r_oval, r_nval;
      std::memcpy(&r_oval, &oldval, sizeof(ET));
      std::memcpy(&r_nval, &newval, sizeof(ET));
      return __sync_bool_compare_and_swap(reinterpret_cast<uint32_t*>(a),
                                          r_oval, r_nval);
    }
  else if
    constexpr(sizeof(ET) == 8) {
      uint64_t r_oval, r_nval;
      std::memcpy(&r_oval, &oldval, sizeof(ET));
      std::memcpy(&r_nval, &newval, sizeof(ET));
      return __sync_bool_compare_and_swap(reinterpret_cast<uint64_t*>(a),
                                          r_oval, r_nval);
    }
  else if
    constexpr(sizeof(ET) == 16) {
      __int128 r_oval, r_nval;
      std::memcpy(&r_oval, &oldval, sizeof(ET));
      std::memcpy(&r_nval, &newval, sizeof(ET));
      return __sync_bool_compare_and_swap_16(reinterpret_cast<__int128*>(a),
                                             r_oval, r_nval);
    }
  else {
    std::cout << "Bad CAS Length" << sizeof(ET) << std::endl;
    exit(0);
  }
}

}  // namespace gbbs

namespace parlay {

inline size_t num_blocks(size_t n, size_t block_size) {
  if (n == 0)
    return 0;
  else
    return (1 + ((n)-1) / (block_size));
}

using parlay::internal::binary_search;

}  // namespace parlay

namespace gbbs {
#define GBBSLONG 1

#ifndef NDEBUG
#define gbbs_debug(_body) _body;
#else
#define gbbs_debug(_body)
#endif

// size of edge-offsets.
// If the number of edges is more than sizeof(MAX_UINT),
// you should set the GBBSLONG flag on the command line.
#if defined(GBBSLONG)
typedef long intT;
typedef unsigned long uintT;
#define INT_T_MAX LONG_MAX
#define UINT_T_MAX ULONG_MAX
#else
typedef int intT;
typedef unsigned int uintT;
#define INT_T_MAX INT_MAX
#define UINT_T_MAX UINT_MAX
#endif

// edge size macros.
// If the number of vertices is more than sizeof(MAX_UINT)
// you should set the GBBSEDGELONG flag on the command line.
#if defined(GBBSEDGELONG)
typedef long intE;
typedef unsigned long uintE;
#define INT_E_MAX LONG_MAX
#define UINT_E_MAX ULONG_MAX
#else
typedef int intE;
typedef unsigned int uintE;
#define INT_E_MAX INT_MAX
#define UINT_E_MAX UINT_MAX
#endif

struct vertex_data {
  size_t offset;  // offset into the edges (static)
  uintE degree;   // possibly decreased by a (mutable) algorithm.
};

// Default granularity of a parallel for loop.
constexpr const size_t kDefaultGranularity = 2048;

// edgemap_sparse_blocked granularity macro
constexpr const size_t kEMBlockSize = 4000;

typedef uint32_t flags;
const flags no_output = 1;
const flags pack_edges = 2;
const flags sparse_blocked = 4;
const flags dense_forward = 8;
const flags dense_parallel = 16;
const flags remove_duplicates = 32;
const flags no_dense = 64;
const flags in_edges = 128;        // map over in edges instead of out edges
const flags fine_parallel = 256;   // split to a node-size of 1
const flags compact_blocks = 512;  // used in SAGE
const flags dense_only = 1024;
inline bool should_output(const flags& fl) { return !(fl & no_output); }

template <class data>
struct vertexSubsetData;

// Specialized version where data = gbbs::empty.
template <>
struct vertexSubsetData<gbbs::empty> {
  using S = uintE;
  using D = bool;

  // Move constructor
  vertexSubsetData(
      vertexSubsetData<gbbs::empty>&& other) noexcept {
    n = other.n;
    m = other.m;
    s = std::move(other.s);
    d = std::move(other.d);
    isDense = other.isDense;
    sum_out_degrees = other.sum_out_degrees;
  }

  // Move assignment
  vertexSubsetData<gbbs::empty>& operator=(
      vertexSubsetData<gbbs::empty>&& other) noexcept {
    if (this != &other) {
      n = other.n;
      m = other.m;
      s = std::move(other.s);
      d = std::move(other.d);
      isDense = other.isDense;
      sum_out_degrees = other.sum_out_degrees;
    }
    return *this;
  }

  // An empty vertex set.
  vertexSubsetData(size_t _n)
      : n(_n),
        m(0),
        isDense(0),
        sum_out_degrees(std::numeric_limits<size_t>::max()) {}

  // A vertexSubset with a single vertex.
  vertexSubsetData(size_t _n, uintE v)
      : n(_n),
        m(1),
        isDense(0),
        sum_out_degrees(std::numeric_limits<size_t>::max()) {
    s = sequence<uintE>(1);
    s[0] = v;
  }

  // A vertexSubset from array of vertex indices.
  vertexSubsetData(size_t _n, size_t _m, sequence<S>&& A)
      : n(_n),
        m(_m),
        s(std::move(A)),
        isDense(0),
        sum_out_degrees(std::numeric_limits<size_t>::max()) {}

  vertexSubsetData(size_t n, sequence<S>&& A)
      : n(n),
        m(A.size()),
        s(std::move(A)),
        isDense(0),
        sum_out_degrees(std::numeric_limits<size_t>::max()) {}

  // A vertexSubset from boolean array giving number of true values.
  vertexSubsetData(size_t _n, size_t _m, sequence<D>&& A)
      : n(_n),
        m(_m),
        d(std::move(A)),
        isDense(1),
        sum_out_degrees(std::numeric_limits<size_t>::max()) {}

  // A vertexSubset from boolean array giving number of true values. Calculate
  // number of nonzeros and store in m.
  vertexSubsetData(size_t _n, sequence<D>&& A)
      : n(_n),
        d(std::move(A)),
        isDense(1),
        sum_out_degrees(std::numeric_limits<size_t>::max()) {
    auto d_f = [&](size_t i) { return d[i]; };
    auto d_map = parlay::delayed_seq<size_t>(n, d_f);
    m = parlay::reduce(d_map);
  }

  bool out_degrees_set() {
    return (sum_out_degrees != std::numeric_limits<size_t>::max());
  }
  size_t get_out_degrees() { return sum_out_degrees; }
  void set_out_degrees(size_t _sum_out_degrees) {
    sum_out_degrees = _sum_out_degrees;
  }

  // Sparse
  inline uintE& vtx(const uintE& i) { return s[i]; }

  // Dense
  __attribute__((always_inline)) inline bool isIn(const uintE& v) const {
    return d[v];
  }

  size_t size() const { return m; }
  size_t numVertices() const { return n; }

  size_t numRows() const { return n; }
  size_t numNonzeros() const { return m; }

  bool isEmpty() const { return m == 0; }
  bool dense() const { return isDense; }

  void toSparse() {
    if (s.size() == 0 && m > 0) {
      auto f_in = parlay::delayed_seq<bool>(n, [&](size_t i) { return d[i]; });
      s = parlay::pack_index<uintE>(f_in);
      if (s.size() != m) {
        std::cout << "# m is " << m << " but out.size says" << s.size()
                  << std::endl;
        std::cout << "# bad stored value of m"
                  << "\n";
        std::cout << "# out.size = " << s.size() << " m = " << m << " n = " << n
                  << "\n";
        abort();
      }
    }
    isDense = false;
  }

  // Converts to dense but keeps sparse representation if it exists.
  void toDense() {
    if (d.size() == 0) {
      d = sequence<bool>(n);
      parallel_for(0, n, [&](size_t i) { d[i] = 0; });
      parallel_for(0, m, [&](size_t i) { d[s[i]] = 1; });
    }
    isDense = true;
  }

  size_t n, m;
  sequence<S> s;
  sequence<D> d;
  bool isDense;
  size_t sum_out_degrees;
};
using vertexSubset = vertexSubsetData<gbbs::empty>;

// Standard version of edgeMapDense.
template <typename data,
          typename std::enable_if<std::is_same<data, gbbs::empty>::value,
                                  int>::type = 0>
inline auto get_emdense_gen(bool* next) {
  return [next](uintE ngh, bool m = false) __attribute__((always_inline)) {
    if (m) next[ngh] = 1;
  };
}

// Standard version of edgeMapDenseForward.
template <typename data,
          typename std::enable_if<std::is_same<data, gbbs::empty>::value,
                                  int>::type = 0>
inline auto get_emdense_forward_gen(bool* next) {
  return [next](uintE ngh, bool m = false) __attribute__((always_inline)) {
    if (m) next[ngh] = 1;
  };
}

template <typename data,
          typename std::enable_if<std::is_same<data, gbbs::empty>::value,
                                  int>::type = 0>
inline auto get_emblock_gen(uintE* outEdges) {
  return [outEdges](uintE ngh, uintT offset, bool m = false)
      __attribute__((always_inline)) {
    if (m) {
      outEdges[offset] = ngh;
      return true;
    }
    return false;
  };
}

// Gen-functions that produce no output
template <typename data,
          typename std::enable_if<std::is_same<data, gbbs::empty>::value,
                                  int>::type = 0>
inline auto get_emsparse_nooutput_gen() {
  return [&](uintE ngh, uintE offset, bool m = false) {};
}

template <typename data>
inline auto get_emsparse_nooutput_gen_empty() {
  return [&](uintE ngh, uintE offset) {};
}

template <typename data,
          typename std::enable_if<std::is_same<data, gbbs::empty>::value,
                                  int>::type = 0>
inline auto get_emdense_nooutput_gen() {
  return [&](uintE ngh, bool m = false) {};
}

template <typename data,
          typename std::enable_if<std::is_same<data, gbbs::empty>::value,
                                  int>::type = 0>
inline auto get_emdense_forward_nooutput_gen() {
  return [&](uintE ngh, bool m = false) {};
}

namespace vertex_ops {

static constexpr uintE kBlockSize = 1024;

}  // namespace vertex_ops

template <class W>
struct uncompressed_neighbors {
  using neighbor_type = std::tuple<uintE, W>;

  uintE id;              // this vertex's id
  uintE degree;          // this vertex's (in/out) degree
  neighbor_type* neighbors;  // the (in/out) neighbors

  uncompressed_neighbors(uintE id, uintE degree, neighbor_type* neighbors)
      : id(id), degree(degree), neighbors(neighbors) {}

  // move constructor
  uncompressed_neighbors(uncompressed_neighbors&& a)
      : id(a.id), degree(a.degree), neighbors(a.neighbors) {}

  uintE get_num_blocks() {
    return parlay::num_blocks(degree, vertex_ops::kBlockSize);
  }

  uintE block_degree(uintE block_num) {
    uintE block_start = block_num * vertex_ops::kBlockSize;
    uintE block_end = std::min(block_start + vertex_ops::kBlockSize, degree);
    return block_end - block_start;
  }

  // ======== Internal primitives used by EdgeMap implementations =======

  // a worker stalls when it starts its first vertex or block of edges in a
  // round, here and in decode_block, the two that BFS's edgeMap uses, at a cost
  // per vertex or block rather than per edge; BFS_F is the only F, and its
  // level numbers the rounds
  template <class VS, class F, class G>
  void decodeBreakEarly(VS& vs, F& f, const G& g, bool parallel = 0) {
    STALL_POINT(parlay::worker_id(), parlay::num_workers(), f.level);
    if (!parallel || degree < 1000) {
      for (size_t j = 0; j < degree; j++) {
        auto nw = neighbors[j];
        uintE ngh = std::get<0>(nw);
        if (vs.isIn(ngh)) {
          auto m = f.update(ngh, id, std::get<1>(nw));
          g(id, m);
        }
        if (!f.cond(id)) break;
      }
    } else {
      size_t b_size = 2048;
      size_t n_blocks = degree / b_size + 1;
      parallel_for(0, n_blocks,
                   [&](size_t b) {
                     if (f.cond(id)) {
                       size_t start = b * b_size;
                       size_t end = std::min((b + 1) * b_size,
                                             static_cast<size_t>(degree));
                       for (size_t j = start; j < end; j++) {
                         if (!f.cond(id)) break;
                         auto nw = neighbors[j];
                         uintE ngh = std::get<0>(nw);
                         if (vs.isIn(ngh)) {
                           auto m = f.updateAtomic(ngh, id, std::get<1>(nw));
                           g(id, m);
                         }
                       }
                     }
                   },
                   1);
    }
  }

  // Used by edgeMapDenseForward. For each out-neighbor satisfying cond, call
  // updateAtomic.
  template <class F, class G>
  void decode(F& f, G& g) {
    parallel_for(0, degree, [&](size_t j) {
      auto nw = neighbors[j];
      uintE ngh = std::get<0>(nw);
      if (f.cond(ngh)) {
        auto m = f.updateAtomic(id, ngh, std::get<1>(nw));
        g(ngh, m);
      }
    });
  }

  // Used by edgeMapSparse. For each out-neighbor satisfying cond, call
  // updateAtomic.
  template <class F, class G, class H>
  void decodeSparse(uintT offset, F& f, const G& g, const H& h,
                    bool parallel = true) {
    size_t granularity =
        parallel ? kDefaultGranularity : std::numeric_limits<size_t>::max();
    parallel_for(0, degree,
                 [&](size_t j) {
                   auto nw = neighbors[j];
                   uintE ngh = std::get<0>(nw);
                   if (f.cond(ngh)) {
                     auto m = f.updateAtomic(id, ngh, std::get<1>(nw));
                     g(ngh, offset + j, m);
                   } else {
                     h(ngh, offset + j);
                   }
                 },
                 granularity);
  }

  // Used in edge_map_blocked.h
  template <class F, class G>
  size_t decode_block(uintT offset, uintE block_num, F& f, const G& g) {
    STALL_POINT(parlay::worker_id(), parlay::num_workers(), f.level);
    size_t k = 0;
    uintE start = vertex_ops::kBlockSize * block_num;
    uintE end = std::min(start + vertex_ops::kBlockSize, degree);
    for (uintE j = start; j < end; j++) {
      auto nw = neighbors[j];
      uintE ngh = std::get<0>(nw);
      if (f.cond(ngh)) {
        auto m = f.updateAtomic(id, ngh, std::get<1>(nw));
        if (g(ngh, offset + k, m)) {  // wrote
          k++;
        }
      }
    }
    return k;
  }

};  // struct uncompressed_neighbors

template <class W>
struct symmetric_vertex {
  using vertex = symmetric_vertex<W>;
  using neighbor_type = std::tuple<uintE, W>;

  uintE id;
  uintE degree;
  neighbor_type* neighbors;

  symmetric_vertex()
      : id(std::numeric_limits<uintE>::max()), degree(0), neighbors(nullptr) {}

  symmetric_vertex(neighbor_type* n, vertex_data vdata, uintE _id) {
    neighbors = (n + vdata.offset);
    degree = vdata.degree;
    id = _id;
  }

  uncompressed_neighbors<W> in_neighbors() {
    return uncompressed_neighbors<W>(id, degree, neighbors);
  }
  uncompressed_neighbors<W> out_neighbors() { return in_neighbors(); }

  uintE in_degree() { return degree; }
  uintE out_degree() { return degree; }

  constexpr static uintE getInternalBlockSize() {
    return vertex_ops::kBlockSize;
  }
  inline uintE in_block_degree(uintE block_num) {
    uintE block_start = block_num * vertex_ops::kBlockSize;
    uintE block_end = std::min(block_start + vertex_ops::kBlockSize, degree);
    return block_end - block_start;  // TODO: check
  }
  inline uintE out_block_degree(uintE block_num) {
    return in_block_degree(block_num);
  }
};

//  Compressed Sparse Row (CSR) based representation for symmetric graphs.
//  Takes two template parameters:
//  1) vertex_type: vertex template, parametrized by the weight type associated
//  with each edge
//  2) W: the edge weight template
//  The graph is represented as an array of edges to neighbors of type
//  vertex_type::neighbor_type.
//  For uncompressed vertices, this type is equal to tuple<uintE, W>.
template <template <class W> class vertex_type, class W>
struct symmetric_graph {
  using vertex = vertex_type<W>;
  using weight_type = W;
  using neighbor_type = typename vertex::neighbor_type;
  using graph = symmetric_graph<vertex_type, W>;

  // Vertices can have an optional weight. Currently the type of this weight is
  // hard-coded as a double. We should consider generalizing this in the future
  // if the need arises.
  using vertex_weight_type = double;

  size_t num_vertices() const { return n; }
  size_t num_edges() const { return m; }

  // ======================= Constructors and fields  ========================
  symmetric_graph()
      : v_data(nullptr),
        e0(nullptr),
        vertex_weights(nullptr),
        n(0),
        m(0),
        deletion_fn([]() {}) {}

  symmetric_graph(vertex_data *v_data, size_t n, size_t m,
                  std::function<void()> &&_deletion_fn, neighbor_type *_e0,
                  vertex_weight_type *_vertex_weights = nullptr)
      : v_data(v_data),
        e0(_e0),
        vertex_weights(_vertex_weights),
        n(n),
        m(m),
        deletion_fn(_deletion_fn) {}

  // Move constructor
  symmetric_graph(symmetric_graph &&other) noexcept {
    n = other.n;
    m = other.m;
    v_data = other.v_data;
    e0 = other.e0;
    vertex_weights = other.vertex_weights;
    deletion_fn = std::move(other.deletion_fn);
    other.v_data = nullptr;
    other.e0 = nullptr;
    other.vertex_weights = nullptr;
    other.deletion_fn = []() {};
  }

  // Move assignment
  symmetric_graph &operator=(symmetric_graph &&other) noexcept {
    deletion_fn();
    n = other.n;
    m = other.m;
    v_data = other.v_data;
    e0 = other.e0;
    vertex_weights = other.vertex_weights;
    deletion_fn = std::move(other.deletion_fn);
    other.v_data = nullptr;
    other.e0 = nullptr;
    other.vertex_weights = nullptr;
    other.deletion_fn = []() {};
    return *this;
  }

  ~symmetric_graph() { deletion_fn(); }

  vertex get_vertex(uintE i) const { return vertex(e0, v_data[i], i); }

  // Graph Data
  vertex_data *v_data;
  // Pointer to edges
  neighbor_type *e0;
  // Pointer to vertex weights
  vertex_weight_type *vertex_weights;

  // number of vertices in G
  size_t n;
  // number of edges in G
  size_t m;

  // called to delete the graph
  std::function<void()> deletion_fn;
};

template <
    class data /* data associated with vertices in the output vertex_subset */,
    class Graph /* graph type */, class VS /* vertex_subset type */,
    class F /* edgeMap struct */>
inline vertexSubsetData<data> edgeMapSparseNoOutput(Graph& G, VS& indices, F& f,
                                                    const flags fl) {
  size_t m = indices.numNonzeros();
  bool inner_parallel = true;

  auto n = G.n;
  auto g = get_emsparse_nooutput_gen<data>();
  auto h = get_emsparse_nooutput_gen_empty<data>();
  parallel_for(0, m,
               [&](size_t i) {
                 uintT v = indices.vtx(i);
                 auto neighbors = (fl & in_edges)
                                      ? G.get_vertex(v).in_neighbors()
                                      : G.get_vertex(v).out_neighbors();
                 neighbors.decodeSparse(0, f, g, h, inner_parallel);
               },
               1);
  return vertexSubsetData<data>(n);
}

struct block {
  uintE id;
  uintE block_num;
  block(uintE _id, uintE _b) : id(_id), block_num(_b) {}
  block() {}
};

constexpr size_t kDataBlockSizeBytes = 16384;
struct em_data_block {
  size_t block_size;
  uint8_t data[kDataBlockSizeBytes];
};

// block format:
// size_t block_size (8 bytes for alignment)
// remainder is used as std::tuple<uintE, data>

template <class data, class Graph>
struct emhelper {
  using thread_blocks = std::vector<em_data_block*>;
  using ngh_data = std::tuple<uintE, data>;
  static constexpr size_t max_block_size =
      kDataBlockSizeBytes / sizeof(ngh_data);

  using vertex = typename Graph::vertex;
  static constexpr size_t work_block_size = vertex::getInternalBlockSize();

  uintE n_groups;
  parlay::sequence<thread_blocks> perthread_blocks;
  parlay::sequence<size_t> perthread_counts;

  static constexpr size_t kPerThreadStride = 128 / sizeof(size_t);
  static constexpr size_t kThreadBlockStride = 1;  // 128/sizeof(thread_blocks);

  emhelper(size_t n_groups) : n_groups(n_groups) {
    perthread_blocks =
        parlay::sequence<thread_blocks>(n_groups * kThreadBlockStride);
    perthread_counts = parlay::sequence<size_t>(n_groups);
  }

  inline size_t scan_perthread_blocks() {
    size_t ct = 0;
    for (size_t i = 0; i < n_groups; i++) {
      size_t i_sz = perthread_blocks[i * kThreadBlockStride].size();
      perthread_counts[i] = ct;
      ct += i_sz;
    }
    return ct;
  }

  auto get_all_blocks() {
    size_t total_blocks = scan_perthread_blocks();
    auto all_blocks = parlay::sequence<em_data_block*>(total_blocks);
    if (total_blocks < 1000) {  // handle sequentially
      size_t k = 0;
      for (size_t i = 0; i < n_groups; i++) {
        auto& vec = perthread_blocks[i * kThreadBlockStride];
        size_t this_thread_size = vec.size();
        for (size_t j = 0; j < this_thread_size; j++) {
          all_blocks[k++] = vec[j];
        }
        vec.clear();
      }
    } else {
      parallel_for(0, n_groups,
                   [&](size_t thread_id) {
                     size_t this_thread_offset = perthread_counts[thread_id];
                     auto& vec =
                         perthread_blocks[thread_id * kThreadBlockStride];
                     size_t this_thread_size = vec.size();
                     for (size_t j = 0; j < this_thread_size; j++) {
                       all_blocks[this_thread_offset + j] = vec[j];
                     }
                     vec.clear();
                   },
                   1);
    }
    return all_blocks;
  }

  // returns the next block for this group, reallocates if nec.
  em_data_block* get_block_and_offset_for_group(size_t group_id) {
    // fetch current block for thread
    em_data_block* block_ptr;
    size_t offset = 0;
    auto& vec = perthread_blocks[group_id * kThreadBlockStride];
    if (vec.size() == 0) {  // alloc new
      block_ptr = gbbs::new_array_no_init<em_data_block>(1);
      vec.emplace_back(block_ptr);
      block_ptr->block_size = 0;
    } else {
      block_ptr = vec.back();
      offset = block_ptr->block_size;
      if (offset + work_block_size > max_block_size) {  // realloc
        block_ptr = gbbs::new_array_no_init<em_data_block>(1);
        vec.emplace_back(block_ptr);
        block_ptr->block_size = 0;
        offset = 0;
      }
    }
    return block_ptr;
  }
};

template <
    class data /* data associated with vertices in the output vertex_subset */,
    class Graph /* graph type */, class VS /* vertex_subset type */,
    class F /* edgeMap struct */>
inline vertexSubsetData<data> edgeMapChunked(Graph& G, VS& indices, F& f,
                                             const flags fl) {
  if (fl & no_output) {
    return edgeMapSparseNoOutput<data, Graph, VS, F>(G, indices, f, fl);
  }
  using S = typename vertexSubsetData<data>::S;
  size_t n = indices.n;

  auto block_f = [&](size_t i) -> size_t {
    uintE vtx_id = indices.vtx(i);
    auto nghs = (fl & in_edges) ? G.get_vertex(vtx_id).in_neighbors()
                                : G.get_vertex(vtx_id).out_neighbors();
    return nghs.get_num_blocks();
  };
  auto block_imap = parlay::delayed_seq<uintE>(indices.size(), block_f);

  // 1. Compute the number of blocks each vertex is subdivided into.
  auto vertex_offs = parlay::sequence<uintE>(indices.size() + 1);
  parallel_for(0, indices.size(),
               [&](size_t i) { vertex_offs[i] = block_imap[i]; });
  vertex_offs[indices.size()] = 0;
  size_t num_blocks = parlay::scan_inplace(make_slice(vertex_offs));

  auto blocks = parlay::sequence<block>(num_blocks);
  auto degrees = parlay::sequence<uintT>(num_blocks);

  // 2. Write each block to blocks and scan degree array.
  parallel_for(0, indices.size(), [&](size_t i) {
    size_t vtx_off = vertex_offs[i];
    size_t num_vertex_blocks = vertex_offs[i + 1] - vtx_off;
    uintE vtx_id = indices.vtx(i);
    assert(vtx_id < n);
    auto neighbors = (fl & in_edges) ? G.get_vertex(vtx_id).in_neighbors()
                                     : G.get_vertex(vtx_id).out_neighbors();
    parallel_for(0, num_vertex_blocks, [&](size_t j) {
      size_t block_deg = neighbors.block_degree(j);
      // assert(block_deg <= PARALLEL_DEGREE); // only for compressed
      blocks[vtx_off + j] = block(i, j);  // j-th block of the i-th vertex.
      degrees[vtx_off + j] = block_deg;
    });
  });
  parlay::scan_inclusive_inplace(make_slice(degrees));
  vertex_offs.clear();
  size_t outEdgeCount = degrees[num_blocks - 1];

  // 3. Compute the number of threads, binary search for offsets.
  // try to use 8*p threads, fewer only if the blocksize guess is smaller than
  // kEMBlockSize
  size_t edge_block_size_guess =
      parlay::num_blocks(outEdgeCount, num_workers() << 3);
  size_t edge_block_size = std::max(kEMBlockSize, edge_block_size_guess);
  size_t n_groups = parlay::num_blocks(outEdgeCount, edge_block_size);

  auto our_emhelper = emhelper<data, Graph>(n_groups);

  // Run each thread in parallel
  auto lt = [](const uintT& l, const uintT& r) { return l < r; };
  parallel_for(
      0, n_groups,
      [&](size_t group_id) {
        size_t start_off = group_id * edge_block_size;
        size_t our_start = parlay::binary_search(degrees, start_off, lt);
        size_t our_end;
        if (group_id < (n_groups - 1)) {
          size_t next_start_off = (group_id + 1) * edge_block_size;
          our_end = parlay::binary_search(degrees, next_start_off, lt);
        } else {
          our_end = num_blocks;
        }

        // <= block_size edges in this range, sequentially process
        if (our_start != our_end && our_start != num_blocks) {
          for (size_t work_id = our_start; work_id < our_end; work_id++) {
            // 1. before starting next work block check whether we need to
            // reallocate
            // the output block. This guarantees that there is enough space in
            // the
            // output block even if all items in the work block are written out
            em_data_block* out_block =
                our_emhelper.get_block_and_offset_for_group(group_id);
            size_t offset = out_block->block_size;
            auto out_block_data = (S*)out_block->data;

            auto g = get_emblock_gen<data>(out_block_data);

            // 2. process the work block
            auto& block = blocks[work_id];
            uintE id = block.id;  // id in vset
            uintE block_num = block.block_num;
            uintE vtx_id =
                indices.vtx(id);  // actual vtx_id corresponding to id
            auto neighbors = (fl & in_edges)
                                 ? G.get_vertex(vtx_id).in_neighbors()
                                 : G.get_vertex(vtx_id).out_neighbors();
            size_t num_in = neighbors.decode_block(offset, block_num, f, g);
            out_block->block_size += num_in;
          }
        }
      },
      1);

  // scan the #output blocks/thread
  parlay::sequence<em_data_block*> all_blocks = our_emhelper.get_all_blocks();
  auto block_offsets = parlay::sequence<size_t>::from_function(
      all_blocks.size(), [&](size_t i) { return all_blocks[i]->block_size; });
  size_t output_size = parlay::scan_inplace(make_slice(block_offsets));
  vertexSubsetData<data> ret(n);
  if (output_size > 0) {
    auto out = parlay::sequence<S>(output_size);

    parallel_for(0, all_blocks.size(),
                 [&](size_t block_id) {
                   em_data_block* block = all_blocks[block_id];
                   size_t block_size = block->block_size;
                   auto block_data = (S*)block->data;
                   size_t block_offset = block_offsets[block_id];
                   for (size_t i = 0; i < block_size; i++) {
                     out[block_offset + i] = block_data[i];
                   }
                   // deallocate block to list_alloc
                   gbbs::free_array(block, 1);
                 },
                 1);
    ret = vertexSubsetData<data>(n, std::move(out));
  } else {
    parallel_for(0, all_blocks.size(), [&](size_t block_id) {
      em_data_block* block = all_blocks[block_id];
      gbbs::free_array(block, 1);
    });
  }
  block_offsets.clear();

  return ret;
}

template <class Data /* per-vertex data in the emitted vertex_subset */,
          class Graph /* graph type */, class VS /* vertex_subset type */,
          class F /* edgeMap struct */>
inline vertexSubsetData<Data> edgeMapDense(Graph& GA, VS& vertexSubset, F& f,
                                           const flags fl) {
  using D = typename vertexSubsetData<Data>::D;
  size_t n = GA.n;
  auto dense_par = fl & dense_parallel;
  if (should_output(fl)) {
    auto next = sequence<D>::from_function(n, [&](size_t i) {
      if
        constexpr(std::is_same<Data, gbbs::empty>()) return 0;
      else
        return std::make_tuple<uintE, Data>(0, Data());
    });
    auto g = get_emdense_gen<Data>(next.begin());
    parallel_for(0, n,
                 [&](size_t v) {
                   if (f.cond(v)) {
                     auto neighbors = (fl & in_edges)
                                          ? GA.get_vertex(v).out_neighbors()
                                          : GA.get_vertex(v).in_neighbors();
                     neighbors.decodeBreakEarly(vertexSubset, f, g, dense_par);
                   }
                 },
                 (fl & fine_parallel) ? 1 : 2048);
    return vertexSubsetData<Data>(n, std::move(next));
  } else {
    auto g = get_emdense_nooutput_gen<Data>();
    parallel_for(0, n,
                 [&](size_t v) {
                   if (f.cond(v)) {
                     auto neighbors = (fl & in_edges)
                                          ? GA.get_vertex(v).out_neighbors()
                                          : GA.get_vertex(v).in_neighbors();
                     neighbors.decodeBreakEarly(vertexSubset, f, g, dense_par);
                   }
                 },
                 (fl & fine_parallel) ? 1 : 2048);
    return vertexSubsetData<Data>(n);
  }
}

template <class Data /* per-vertex data in the emitted vertex_subset */,
          class Graph /* graph type */, class VS /* vertex_subset type */,
          class F /* edgeMap struct */>
inline vertexSubsetData<Data> edgeMapDenseForward(Graph& GA, VS& vertexSubset,
                                                  F& f, const flags fl) {
  gbbs_debug(std::cout << "# dense forward" << std::endl;);
  using D = typename vertexSubsetData<Data>::D;
  size_t n = GA.n;
  if (should_output(fl)) {
    auto next = sequence<D>(n);
    auto g = get_emdense_forward_gen<Data>(next.begin());
    if
      constexpr(std::is_same<Data, gbbs::empty>()) {
        parallel_for(0, n, [&](size_t i) { next[i] = 0; }, kDefaultGranularity);
      }
    else {
      parallel_for(0, n, [&](size_t i) { std::get<0>(next[i]) = 0; },
                   kDefaultGranularity);
    }
    parallel_for(0, n,
                 [&](size_t i) {
                   if (vertexSubset.isIn(i)) {
                     auto neighbors = (fl & in_edges)
                                          ? GA.get_vertex(i).in_neighbors()
                                          : GA.get_vertex(i).out_neighbors();
                     neighbors.decode(f, g);
                   }
                 },
                 1);
    return vertexSubsetData<Data>(n, std::move(next));
  } else {
    auto g = get_emdense_forward_nooutput_gen<Data>();
    parallel_for(0, n,
                 [&](size_t i) {
                   if (vertexSubset.isIn(i)) {
                     auto neighbors = (fl & in_edges)
                                          ? GA.get_vertex(i).in_neighbors()
                                          : GA.get_vertex(i).out_neighbors();
                     neighbors.decode(f, g);
                   }
                 },
                 1);
    return vertexSubsetData<Data>(n);
  }
}

// Decides on sparse or dense base on number of nonzeros in the active vertices.
template <
    class Data /* data associated with vertices in the output vertex_subset */,
    class Graph /* graph type */, class VS /* vertex_subset type */,
    class F /* edgeMap struct */>
inline vertexSubsetData<Data> edgeMapData(Graph& GA, VS& vs, F f,
                                          intT threshold = -1,
                                          const flags& fl = 0) {
  size_t numVertices = GA.n, numEdges = GA.m, m = vs.numNonzeros();
  size_t dense_threshold = threshold;
  if (threshold == -1) dense_threshold = numEdges / 20;
  if (vs.size() == 0) return vertexSubsetData<Data>(numVertices);

  if ((fl & dense_only) || (vs.isDense && vs.size() > numVertices / 10)) {
    vs.toDense();
    timer dt;
    dt.start();
    auto ret = (fl & dense_forward)
                   ? edgeMapDenseForward<Data, Graph, VS, F>(GA, vs, f, fl)
                   : edgeMapDense<Data, Graph, VS, F>(GA, vs, f, fl);
    dt.stop();
    gbbs_debug(dt.next("dense time"););
    return ret;
  }

  timer st;
  st.start();
  size_t out_degrees = 0;
  if (vs.out_degrees_set()) {
    out_degrees = vs.get_out_degrees();
  } else {
    vs.toSparse();
    auto degree_f = [&](size_t i) {
      return (fl & in_edges) ? GA.get_vertex(vs.vtx(i)).in_degree()
                             : GA.get_vertex(vs.vtx(i)).out_degree();
    };
    auto degree_im = parlay::delayed_seq<size_t>(vs.size(), degree_f);
    out_degrees = parlay::reduce(degree_im);
    vs.set_out_degrees(out_degrees);
  }

  if (out_degrees == 0) return vertexSubsetData<Data>(numVertices);
  if (m + out_degrees > dense_threshold && !(fl & no_dense)) {
    vs.toDense();
    auto ret = (fl & dense_forward)
                   ? edgeMapDenseForward<Data, Graph, VS, F>(GA, vs, f, fl)
                   : edgeMapDense<Data, Graph, VS, F>(GA, vs, f, fl);
    st.stop();
    gbbs_debug(st.next("dense convert time"););
    return ret;
  } else {
    auto vs_out = edgeMapChunked<Data, Graph, VS, F>(GA, vs, f, fl);
    st.stop();
    gbbs_debug(st.next("sparse time"););
    return vs_out;
  }
}

// Regular edgeMap, where no extra data is stored per vertex.
template <class Graph /* graph type */, class VS /* vertex_subset type */,
          class F /* edgeMap struct */>
inline vertexSubset edgeMap(Graph& GA, VS& vs, F f, intT threshold = -1,
                            const flags& fl = 0) {
  return edgeMapData<gbbs::empty>(GA, vs, f, threshold, fl);
}


// GBBS's BFS kernel (benchmarks/BFS/NonDeterministicBFS/BFS.h), storing each
// vertex's level where it stored its parent, and without printing each
// frontier's size

template <class W>
struct BFS_F {
  uintE* Dist;
  uintE level;
  BFS_F(uintE* _Dist, uintE _level) : Dist(_Dist), level(_level) {}
  inline bool update(uintE s, uintE d, W w) {
    if (Dist[d] == UINT_E_MAX) {
      Dist[d] = level;
      return 1;
    } else {
      return 0;
    }
  }
  inline bool updateAtomic(uintE s, uintE d, W w) {
    return (gbbs::atomic_compare_and_swap(&Dist[d], UINT_E_MAX, level));
  }
  inline bool cond(uintE d) { return (Dist[d] == UINT_E_MAX); }
};

// when the traversal of the last search began and ended, which split its time
// into the setup before the traversal, the traversal, and what follows it
inline std::chrono::high_resolution_clock::time_point traversal_began, traversal_ended;

template <class Graph>
inline sequence<uintE> BFS(Graph& G, uintE src) {
  using W = typename Graph::weight_type;
  /* Creates Dist array, initialized to all -1, except for src. */
  auto Dist =
      sequence<uintE>::from_function(G.n, [&](size_t i) { return UINT_E_MAX; });
  Dist[src] = 0;

  vertexSubset Frontier(G.n, src);
  uintE level = 0;
  traversal_began = std::chrono::high_resolution_clock::now();
  while (!Frontier.isEmpty()) {
    level++;
    Frontier = edgeMap(G, Frontier, BFS_F<W>(Dist.begin(), level), -1,
                       sparse_blocked | dense_parallel);
  }
  traversal_ended = std::chrono::high_resolution_clock::now();
  return Dist;
}

}  // namespace gbbs


// Driver: kept the same as wf-bfs.cpp's, so the two are timed and reported
// the same way

using namespace std;
using namespace chrono;

using gbbs_graph = gbbs::symmetric_graph<gbbs::symmetric_vertex, gbbs::empty>;

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

// the CSR the shared reader builds, copied into arrays GBBS's graph owns, on
// huge pages as the reader's own arrays are. Neighbors keep file order and
// duplicates, as in wf-bfs; every edge is listed at both its ends, as GBBS's
// symmetric graphs list them
[[gnu::noinline]] unique_ptr<gbbs_graph> load_graph(const string& path) {
    graph_helpers::array<size_t> offsets;
    graph_helpers::array<int> adj;
    {
        graph_helpers::edge_list edges = graph_helpers::read_graph(path);
        N = edges.n;
        M = edges.from.size();
        graph_helpers::build_csr(edges, offsets, adj);
    }

    using neighbor = gbbs_graph::neighbor_type;
    const size_t n = static_cast<size_t>(N);
    const size_t total = adj.size();
    gbbs::vertex_data* v_data = gbbs::new_array_no_init<gbbs::vertex_data>(n);
    neighbor* e0 = gbbs::new_array_no_init<neighbor>(total);
    graph_helpers::advise_huge_pages(v_data, sizeof(gbbs::vertex_data) * n);
    graph_helpers::advise_huge_pages(e0, sizeof(neighbor) * total);

    const int threads = graph_helpers::load_threads;
    graph_helpers::detail::run_on_threads(threads, [&](int t) {
        for(size_t i=total*t/threads; i<total*(t+1)/threads; i++) {
            new(e0 + i) neighbor(static_cast<gbbs::uintE>(adj[i]), gbbs::empty());
        }
        for(size_t u=n*t/threads; u<n*(t+1)/threads; u++) {
            v_data[u].offset = offsets[u];
            v_data[u].degree = static_cast<gbbs::uintE>(offsets[u+1] - offsets[u]);
        }
    });

    return unique_ptr<gbbs_graph>(new gbbs_graph(
        v_data, n, total,
        [=]() {
            gbbs::free_array(v_data, n);
            gbbs::free_array(e0, total);
        },
        e0));
}

// the ids of the process's threads, the main thread's first
[[gnu::noinline]] vector<pid_t> list_threads() {
    vector<pid_t> tids;
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
    sort(tids.begin(), tids.end());
    auto main_thread = find(tids.begin(), tids.end(), getpid());
    if(main_thread != tids.end()) rotate(tids.begin(), main_thread, main_thread + 1);
    return tids;
}

// pins thread tid, not necessarily the calling one, to cpu and names it
[[gnu::noinline]] bool pin_thread(pid_t tid, int cpu, const string& name) {
    if(cpu < 0 || cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if(sched_setaffinity(tid, sizeof(set), &set) != 0) return false;
    ofstream("/proc/self/task/" + to_string(tid) + "/comm") << name;
    return true;
}

// a ParlayLib scheduler starts all its workers when made, the calling thread
// being worker 0, and keeps them for every parallel loop it runs, so pinning
// them once holds for all repetitions. Its idle workers sleep until new work
// wakes one, and a wake-up can be missed, so parallel tasks are not sure to
// reach every worker; by now the process's threads are just the workers, so
// they are pinned by their thread ids instead
[[gnu::noinline]] bool start_threads(const vector<int>& cpu_ids) {
    const vector<pid_t> threads = list_threads();
    if(static_cast<int>(threads.size()) != num_t) return false;
    for(int i=0; i<num_t; i++) {
        if(!pin_thread(threads[i], cpu_ids[i], "gbbs-" + to_string(i))) return false;
    }
    return true;
}

[[gnu::noinline]] void write_output(const vector<int>& dist) {
    graph_helpers::write_distances("gbbs-out.txt", dist);
}

[[gnu::noinline]] int main(int argc, char *argv[]) {
    ios_base::sync_with_stdio(false); cin.tie(0); cout.tie(0);

    if(argc != 3) {
        cout << "Usage: " << argv[0] << " <input_file> <num_threads>[,<num_threads>...]\n";
        return 1;
    }

    vector<int> thread_counts;
    try {
        thread_counts = cpu_helpers::parse_thread_counts(argv[2]);
    }
    catch(const exception& error) {
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

    unique_ptr<gbbs_graph> g;
    gbbs::uintE source = 0;
    try {
        g = load_graph(argv[1]);
        source = static_cast<gbbs::uintE>(graph_helpers::search_source(g->num_vertices()));
    }
    catch(const exception& error) {
        cerr << "Error: " << error.what() << "\n";
        return 1;
    }

    // given several thread counts, a line for each, its distances checked
    // against the first one's; the last one's are written
    vector<int> first_dist;
    vector<int> dist;
    long long average_duration = 0;
    for(size_t c = 0; c < thread_counts.size(); c++) {
        if(c > 0) this_thread::sleep_for(cpu_helpers::pause_between_thread_counts);
        num_t = thread_counts[c];

        vector<int> cpu_ids;
        try {
            cpu_ids = cpu_helpers::select_idle_cpus(num_t);
        }
        catch(const exception& error) {
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

            constexpr int repetitions = 20;
            long long total_duration = 0;
            // the setup before the traversal, which takes the place of our reset
            // in the phases line, and the traversal
            long long total_setup = 0;
            long long total_traversal = 0;
            gbbs::sequence<gbbs::uintE> last;

            control_perf("enable");
            // the process's threads are still just the workers (see start_threads),
            // until noise starts its own
            stalls::sched_watch watch(list_threads());
            stalls::noise noise(list_threads());
            stalls::search_times times;

            for(int i=0; i<repetitions; i++) {
                high_resolution_clock::time_point t1 = high_resolution_clock::now();
                gbbs::sequence<gbbs::uintE> result = gbbs::BFS(*g, source);
                high_resolution_clock::time_point t2 = high_resolution_clock::now();

                total_duration += duration_cast<microseconds>(t2 - t1).count();
                total_setup += duration_cast<microseconds>(gbbs::traversal_began - t1).count();
                total_traversal += duration_cast<microseconds>(
                    gbbs::traversal_ended - gbbs::traversal_began).count();
                times.add(duration_cast<microseconds>(t2 - t1).count());
                swap(last, result);
            }

            noise.stop();
            watch.report(repetitions);
            times.report();
            cerr << "phases reset_us " << total_setup / repetitions
                 << " search_us " << total_traversal / repetitions << "\n";
            control_perf("disable");

            average_duration = total_duration / repetitions;
            dist.assign(last.begin(), last.end());
            ran = true;
        });
        if(!ran) return 1;

        if(thread_counts.size() > 1) {
            if(c == 0) first_dist = dist;
            cout << num_t << " " << average_duration << " "
                 << (dist == first_dist ? "same" : "differs") << endl;
        }
    }

    write_output(dist);

    if(thread_counts.size() == 1) cout << average_duration << "\n";

    return 0;
}

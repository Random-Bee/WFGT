// DO_BFS/dir-bfs.cpp with the stalls of stall.hpp, and the time of its
// traversal apart from the setup before it, for stalls.sh, noise.sh and
// diag128.sh
// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#include <bits/stdc++.h>
#include <omp.h>
#include <sys/time.h>
#include "../../cpu_helpers/cpu_affinity.hpp"
#include "../../graph_helpers/graph_reader.hpp"
#include "stall.hpp"


// The parts of GAPBS's support headers (platform_atomics.h, pvector.h,
// bitmap.h, sliding_queue.h, graph.h, timer.h, util.h) that the kernel uses

template<typename T, typename U>
T fetch_and_add(T &x, U inc) {
  return __sync_fetch_and_add(&x, inc);
}

template<typename T>
bool compare_and_swap(T &x, const T &old_val, const T &new_val) {
  return __sync_bool_compare_and_swap(&x, old_val, new_val);
}


template <typename T_>
class pvector {
 public:
  typedef T_* iterator;

  pvector() : start_(nullptr), end_size_(nullptr), end_capacity_(nullptr) {}

  explicit pvector(size_t num_elements) {
    start_ = new T_[num_elements];
    end_size_ = start_ + num_elements;
    end_capacity_ = end_size_;
  }

  // don't want this to be copied, too much data to move
  pvector(const pvector &other) = delete;

  // prefer move because too much data to copy
  pvector(pvector &&other)
      : start_(other.start_), end_size_(other.end_size_),
        end_capacity_(other.end_capacity_) {
    other.start_ = nullptr;
    other.end_size_ = nullptr;
    other.end_capacity_ = nullptr;
  }

  // want move assignment
  pvector& operator= (pvector &&other) {
    if (this != &other) {
      ReleaseResources();
      start_ = other.start_;
      end_size_ = other.end_size_;
      end_capacity_ = other.end_capacity_;
      other.start_ = nullptr;
      other.end_size_ = nullptr;
      other.end_capacity_ = nullptr;
    }
    return *this;
  }

  void ReleaseResources(){
    if (start_ != nullptr) {
      delete[] start_;
    }
  }

  ~pvector() {
    ReleaseResources();
  }

  T_& operator[](size_t n) {
    return start_[n];
  }

  const T_& operator[](size_t n) const {
    return start_[n];
  }

  size_t size() const {
    return end_size_ - start_;
  }

  void swap(pvector &other) {
    std::swap(start_, other.start_);
    std::swap(end_size_, other.end_size_);
    std::swap(end_capacity_, other.end_capacity_);
  }

  iterator begin() const {
    return start_;
  }

  iterator end() const {
    return end_size_;
  }

 private:
  T_* start_;
  T_* end_size_;
  T_* end_capacity_;
};


class Bitmap {
 public:
  explicit Bitmap(size_t size) {
    uint64_t num_words = (size + kBitsPerWord - 1) / kBitsPerWord;
    start_ = new uint64_t[num_words];
    end_ = start_ + num_words;
  }

  ~Bitmap() {
    delete[] start_;
  }

  void reset() {
    std::fill(start_, end_, 0);
  }

  void set_bit(size_t pos) {
    start_[word_offset(pos)] |= ((uint64_t) 1l << bit_offset(pos));
  }

  void set_bit_atomic(size_t pos) {
    uint64_t old_val, new_val;
    do {
      old_val = start_[word_offset(pos)];
      new_val = old_val | ((uint64_t) 1l << bit_offset(pos));
    } while (!compare_and_swap(start_[word_offset(pos)], old_val, new_val));
  }

  bool get_bit(size_t pos) const {
    return (start_[word_offset(pos)] >> bit_offset(pos)) & 1l;
  }

  void swap(Bitmap &other) {
    std::swap(start_, other.start_);
    std::swap(end_, other.end_);
  }

 private:
  uint64_t *start_;
  uint64_t *end_;

  static const uint64_t kBitsPerWord = 64;
  static uint64_t word_offset(size_t n) { return n / kBitsPerWord; }
  static uint64_t bit_offset(size_t n) { return n & (kBitsPerWord - 1); }
};


template <typename T>
class QueueBuffer;

template <typename T>
class SlidingQueue {
  T *shared;
  size_t shared_in;
  size_t shared_out_start;
  size_t shared_out_end;
  friend class QueueBuffer<T>;

 public:
  explicit SlidingQueue(size_t shared_size) {
    shared = new T[shared_size];
    reset();
  }

  ~SlidingQueue() {
    delete[] shared;
  }

  void push_back(T to_add) {
    shared[shared_in++] = to_add;
  }

  bool empty() const {
    return shared_out_start == shared_out_end;
  }

  void reset() {
    shared_out_start = 0;
    shared_out_end = 0;
    shared_in = 0;
  }

  void slide_window() {
    shared_out_start = shared_out_end;
    shared_out_end = shared_in;
  }

  typedef T* iterator;

  iterator begin() const {
    return shared + shared_out_start;
  }

  iterator end() const {
    return shared + shared_out_end;
  }

  size_t size() const {
    return end() - begin();
  }
};


template <typename T>
class QueueBuffer {
  size_t in;
  T *local_queue;
  SlidingQueue<T> &sq;
  const size_t local_size;

 public:
  explicit QueueBuffer(SlidingQueue<T> &master, size_t given_size = 16384)
      : sq(master), local_size(given_size) {
    in = 0;
    local_queue = new T[local_size];
  }

  ~QueueBuffer() {
    delete[] local_queue;
  }

  void push_back(T to_add) {
    if (in == local_size)
      flush();
    local_queue[in++] = to_add;
  }

  void flush() {
    T *shared_queue = sq.shared;
    size_t copy_start = fetch_and_add(sq.shared_in, in);
    std::copy(local_queue, local_queue+in, shared_queue+copy_start);
    in = 0;
  }
};


template <class NodeID_, class DestID_ = NodeID_, bool MakeInverse = true>
class CSRGraph {
  // Used for *non-negative* offsets within a neighborhood
  typedef std::make_unsigned<std::ptrdiff_t>::type OffsetT;

  // Used to access neighbors of vertex, basically sugar for iterators
  class Neighborhood {
    NodeID_ n_;
    DestID_** g_index_;
    OffsetT start_offset_;
   public:
    Neighborhood(NodeID_ n, DestID_** g_index, OffsetT start_offset) :
        n_(n), g_index_(g_index), start_offset_(0) {
      OffsetT max_offset = end() - begin();
      start_offset_ = std::min(start_offset, max_offset);
    }
    typedef DestID_* iterator;
    iterator begin() { return g_index_[n_] + start_offset_; }
    iterator end()   { return g_index_[n_+1]; }
  };

  void ReleaseResources() {
    if (out_index_ != nullptr)
      delete[] out_index_;
    if (out_neighbors_ != nullptr)
      delete[] out_neighbors_;
    if (directed_) {
      if (in_index_ != nullptr)
        delete[] in_index_;
      if (in_neighbors_ != nullptr)
        delete[] in_neighbors_;
    }
  }

 public:
  CSRGraph() : directed_(false), num_nodes_(-1), num_edges_(-1),
    out_index_(nullptr), out_neighbors_(nullptr),
    in_index_(nullptr), in_neighbors_(nullptr) {}

  CSRGraph(int64_t num_nodes, DestID_** index, DestID_* neighs) :
    directed_(false), num_nodes_(num_nodes),
    out_index_(index), out_neighbors_(neighs),
    in_index_(index), in_neighbors_(neighs) {
      num_edges_ = (out_index_[num_nodes_] - out_index_[0]) / 2;
    }

  CSRGraph(CSRGraph&& other) : directed_(other.directed_),
    num_nodes_(other.num_nodes_), num_edges_(other.num_edges_),
    out_index_(other.out_index_), out_neighbors_(other.out_neighbors_),
    in_index_(other.in_index_), in_neighbors_(other.in_neighbors_) {
      other.num_edges_ = -1;
      other.num_nodes_ = -1;
      other.out_index_ = nullptr;
      other.out_neighbors_ = nullptr;
      other.in_index_ = nullptr;
      other.in_neighbors_ = nullptr;
  }

  ~CSRGraph() {
    ReleaseResources();
  }

  bool directed() const {
    return directed_;
  }

  int64_t num_nodes() const {
    return num_nodes_;
  }

  int64_t num_edges() const {
    return num_edges_;
  }

  int64_t num_edges_directed() const {
    return directed_ ? num_edges_ : 2*num_edges_;
  }

  int64_t out_degree(NodeID_ v) const {
    return out_index_[v+1] - out_index_[v];
  }

  int64_t in_degree(NodeID_ v) const {
    static_assert(MakeInverse, "Graph inversion disabled but reading inverse");
    return in_index_[v+1] - in_index_[v];
  }

  Neighborhood out_neigh(NodeID_ n, OffsetT start_offset = 0) const {
    return Neighborhood(n, out_index_, start_offset);
  }

  Neighborhood in_neigh(NodeID_ n, OffsetT start_offset = 0) const {
    static_assert(MakeInverse, "Graph inversion disabled but reading inverse");
    return Neighborhood(n, in_index_, start_offset);
  }

 private:
  bool directed_;
  int64_t num_nodes_;
  int64_t num_edges_;
  DestID_** out_index_;
  DestID_*  out_neighbors_;
  DestID_** in_index_;
  DestID_*  in_neighbors_;
};


class Timer {
 public:
  Timer() {}

  void Start() {
    gettimeofday(&start_time_, NULL);
  }

  void Stop() {
    gettimeofday(&elapsed_time_, NULL);
    elapsed_time_.tv_sec  -= start_time_.tv_sec;
    elapsed_time_.tv_usec -= start_time_.tv_usec;
  }

  double Seconds() const {
    return elapsed_time_.tv_sec + elapsed_time_.tv_usec/1e6;
  }

 private:
  struct timeval start_time_;
  struct timeval elapsed_time_;
};

// Times op's execution using the timer t
#define TIME_OP(t, op) { t.Start(); (op); t.Stop(); }


void PrintStep(const std::string &s, int64_t count) {
  printf("%-14s%14" PRId64 "\n", (s + ":").c_str(), count);
}

void PrintStep(const std::string &s, double seconds, int64_t count = -1) {
  if (count != -1)
    printf("%5s%11" PRId64 "  %10.5lf\n", s.c_str(), count, seconds);
  else
    printf("%5s%23.5lf\n", s.c_str(), seconds);
}


typedef int32_t NodeID;
typedef CSRGraph<NodeID> Graph;


/*
GAP Benchmark Suite
Kernel: Breadth-First Search (BFS)
Author: Scott Beamer

Will return distance array for a BFS traversal from a source vertex

This BFS implementation makes use of the Direction-Optimizing approach [1].
It uses the alpha and beta parameters to determine whether to switch search
directions. For representing the frontier, it uses a SlidingQueue for the
top-down approach and a Bitmap for the bottom-up approach. To reduce
false-sharing for the top-down approach, thread-local QueueBuffer's are used.

To save time computing the number of edges exiting the frontier, this
implementation precomputes the degrees in bulk at the beginning by storing
them in the distance array as negative numbers. Thus, the encoding of dist is:
  dist[x] < 0 implies x is unvisited and dist[x] = -out_degree(x)
  dist[x] >= 0 implies x been visited, at depth dist[x]

[1] Scott Beamer, Krste Asanović, and David Patterson. "Direction-Optimizing
    Breadth-First Search." International Conference on High Performance
    Computing, Networking, Storage and Analysis (SC), Salt Lake City, Utah,
    November 2012.
*/


using namespace std;


int64_t BUStep(const Graph &g, pvector<NodeID> &dist, Bitmap &front,
               Bitmap &next, NodeID depth) {
  int64_t awake_count = 0;
  next.reset();
  // a stalled thread waits once per step, before it takes a chunk, so the loop
  // gets a parallel region of its own to wait in
  #pragma omp parallel reduction(+ : awake_count)
  {
  STALL_POINT(omp_get_thread_num(), omp_get_num_threads(), depth);
  #pragma omp for schedule(dynamic, 1024)
  for (NodeID u=0; u < g.num_nodes(); u++) {
    if (dist[u] < 0) {
      for (NodeID v : g.in_neigh(u)) {
        if (front.get_bit(v)) {
          dist[u] = depth;
          awake_count++;
          next.set_bit(u);
          break;
        }
      }
    }
  }
  }
  return awake_count;
}


int64_t TDStep(const Graph &g, pvector<NodeID> &dist,
               SlidingQueue<NodeID> &queue, NodeID depth) {
  int64_t scout_count = 0;
  #pragma omp parallel
  {
    QueueBuffer<NodeID> lqueue(queue);
    STALL_POINT(omp_get_thread_num(), omp_get_num_threads(), depth);
    #pragma omp for reduction(+ : scout_count) nowait
    for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
      NodeID u = *q_iter;
      for (NodeID v : g.out_neigh(u)) {
        NodeID curr_val = dist[v];
        if (curr_val < 0) {
          if (compare_and_swap(dist[v], curr_val, depth)) {
            lqueue.push_back(v);
            scout_count += -curr_val;
          }
        }
      }
    }
    lqueue.flush();
  }
  return scout_count;
}


void QueueToBitmap(const SlidingQueue<NodeID> &queue, Bitmap &bm) {
  #pragma omp parallel for
  for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
    NodeID u = *q_iter;
    bm.set_bit_atomic(u);
  }
}

void BitmapToQueue(const Graph &g, const Bitmap &bm,
                   SlidingQueue<NodeID> &queue) {
  #pragma omp parallel
  {
    QueueBuffer<NodeID> lqueue(queue);
    #pragma omp for nowait
    for (NodeID n=0; n < g.num_nodes(); n++)
      if (bm.get_bit(n))
        lqueue.push_back(n);
    lqueue.flush();
  }
  queue.slide_window();
}

pvector<NodeID> InitDist(const Graph &g) {
  pvector<NodeID> dist(g.num_nodes());
  #pragma omp parallel for
  for (NodeID n=0; n < g.num_nodes(); n++)
    dist[n] = g.out_degree(n) != 0 ? -g.out_degree(n) : -1;
  return dist;
}

// when the traversal of the last search began and ended, which split its time
// into the setup before the traversal, the traversal, and what follows it
std::chrono::high_resolution_clock::time_point traversal_began, traversal_ended;

pvector<NodeID> DOBFS(const Graph &g, NodeID source, bool logging_enabled = false,
                      int alpha = 15, int beta = 18) {
  if (logging_enabled)
    PrintStep("Source", static_cast<int64_t>(source));
  Timer t;
  t.Start();
  pvector<NodeID> dist = InitDist(g);
  t.Stop();
  if (logging_enabled)
    PrintStep("i", t.Seconds());
  dist[source] = 0;
  // every step, top-down or bottom-up, discovers exactly the next level
  NodeID depth = 0;
  SlidingQueue<NodeID> queue(g.num_nodes());
  queue.push_back(source);
  queue.slide_window();
  Bitmap curr(g.num_nodes());
  curr.reset();
  Bitmap front(g.num_nodes());
  front.reset();
  int64_t edges_to_check = g.num_edges_directed();
  int64_t scout_count = g.out_degree(source);
  traversal_began = std::chrono::high_resolution_clock::now();
  while (!queue.empty()) {
    if (scout_count > edges_to_check / alpha) {
      int64_t awake_count, old_awake_count;
      TIME_OP(t, QueueToBitmap(queue, front));
      if (logging_enabled)
        PrintStep("e", t.Seconds());
      awake_count = queue.size();
      queue.slide_window();
      do {
        t.Start();
        old_awake_count = awake_count;
        depth++;
        awake_count = BUStep(g, dist, front, curr, depth);
        front.swap(curr);
        t.Stop();
        if (logging_enabled)
          PrintStep("bu", t.Seconds(), awake_count);
      } while ((awake_count >= old_awake_count) ||
               (awake_count > g.num_nodes() / beta));
      TIME_OP(t, BitmapToQueue(g, front, queue));
      if (logging_enabled)
        PrintStep("c", t.Seconds());
      scout_count = 1;
    } else {
      t.Start();
      edges_to_check -= scout_count;
      depth++;
      scout_count = TDStep(g, dist, queue, depth);
      queue.slide_window();
      t.Stop();
      if (logging_enabled)
        PrintStep("td", t.Seconds(), queue.size());
    }
  }
  traversal_ended = std::chrono::high_resolution_clock::now();
  #pragma omp parallel for
  for (NodeID n = 0; n < g.num_nodes(); n++)
    if (dist[n] < -1)
      dist[n] = -1;
  return dist;
}


// Driver: kept the same as wf-bfs.cpp's, so the two are timed and reported
// the same way

using namespace chrono;

int N;
size_t M;

int num_t;

[[gnu::noinline]] void name_current_thread(const char* role, int tid) {
    char name[16];
    snprintf(name, sizeof(name), "%s-%d", role, tid);
    pthread_setname_np(pthread_self(), name);
}

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

// the CSR the shared reader builds, copied into arrays GAPBS's graph owns, on
// huge pages as the reader's own arrays are. Neighbors keep file order and
// duplicates, as in wf-bfs; GAPBS's own builder would sort and deduplicate them
[[gnu::noinline]] Graph load_graph(const string& path) {
    graph_helpers::array<size_t> offsets;
    graph_helpers::array<int> adj;
    {
        graph_helpers::edge_list edges = graph_helpers::read_graph(path);
        N = edges.n;
        M = edges.from.size();
        graph_helpers::build_csr(edges, offsets, adj);
    }

    const int64_t total = static_cast<int64_t>(adj.size());
    const int threads = graph_helpers::load_threads;
    NodeID* neighs = new NodeID[total];
    graph_helpers::advise_huge_pages(neighs, sizeof(NodeID) * total);
    #pragma omp parallel for num_threads(threads)
    for(int t=0; t<threads; t++) {
        copy(adj.begin() + total * t / threads,
             adj.begin() + total * (t + 1) / threads,
             neighs + total * t / threads);
    }
    NodeID** index = new NodeID*[N + 1];
    graph_helpers::advise_huge_pages(index, sizeof(NodeID*) * (N + 1));
    for(int u=0; u<=N; u++) index[u] = neighs + offsets[u];

    return Graph(N, index, neighs);
}

// the threads' ids, which stalls at random times need
vector<pid_t> worker_ids;

// libgomp reuses the same threads for every later parallel region, so pinning
// them once holds for all repetitions
[[gnu::noinline]] bool start_threads(const vector<int>& cpu_ids) {
    omp_set_dynamic(0);
    omp_set_num_threads(num_t);

    vector<int> pinned(num_t, 0);
    worker_ids.assign(num_t, 0);
    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        name_current_thread("do", tid);
        try {
            cpu_helpers::pin_current_thread(cpu_ids[tid]);
            pinned[tid] = 1;
        }
        catch(const exception&) {}
        worker_ids[tid] = stalls::current_thread();
    }
    return count(pinned.begin(), pinned.end(), 1) == num_t;
}

[[gnu::noinline]] void write_output(const pvector<NodeID>& dist) {
    graph_helpers::write_distances("dir-out.txt", vector<int>(dist.begin(), dist.end()));
}

[[gnu::noinline]] int main(int argc, char *argv[]) {
    ios_base::sync_with_stdio(false); cin.tie(0); cout.tie(0);

    if(argc != 3) {
        cout << "Usage: " << argv[0] << " <input_file> <num_threads>[,<num_threads>...]\n";
        return 1;
    }

    // the graph can be moved from but not assigned
    unique_ptr<Graph> g;
    NodeID source = 0;
    try {
        g.reset(new Graph(load_graph(argv[1])));
        source = graph_helpers::search_source(static_cast<size_t>(g->num_nodes()));
    }
    catch(const exception& error) {
        cerr << "Error: " << error.what() << "\n";
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

    // given several thread counts, a line for each, its distances checked
    // against the first one's; the last one's are written
    vector<int> first_dist;
    pvector<NodeID> dist;
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

        if(!start_threads(cpu_ids)) {
            cerr << "Error: Failed to pin threads to the selected CPUs\n";
            return 1;
        }

        constexpr int repetitions = 20;
        long long total_duration = 0;
        // the setup before the traversal, which takes the place of our reset
        // in the phases line, and the traversal
        long long total_setup = 0;
        long long total_traversal = 0;

        control_perf("enable");
        stalls::sched_watch watch(worker_ids);
        stalls::noise noise(worker_ids);
        stalls::search_times times;

        for(int i=0; i<repetitions; i++) {
            high_resolution_clock::time_point t1 = high_resolution_clock::now();
            pvector<NodeID> result = DOBFS(*g, source);
            high_resolution_clock::time_point t2 = high_resolution_clock::now();

            total_duration += duration_cast<microseconds>(t2 - t1).count();
            total_setup += duration_cast<microseconds>(traversal_began - t1).count();
            total_traversal += duration_cast<microseconds>(traversal_ended - traversal_began).count();
            times.add(duration_cast<microseconds>(t2 - t1).count());
            dist.swap(result);
        }

        noise.stop();
        watch.report(repetitions);
        times.report();
        cerr << "phases reset_us " << total_setup / repetitions
             << " search_us " << total_traversal / repetitions << "\n";
        control_perf("disable");

        average_duration = total_duration / repetitions;
        if(thread_counts.size() > 1) {
            vector<int> distances(dist.begin(), dist.end());
            if(c == 0) first_dist = distances;
            cout << num_t << " " << average_duration << " "
                 << (distances == first_dist ? "same" : "differs") << endl;
        }
    }

    write_output(dist);

    if(thread_counts.size() == 1) cout << average_duration << "\n";

    return 0;
}

#include <bits/stdc++.h>
#include "../cpu_helpers/cpu_affinity.hpp"
#include "../graph_helpers/graph_reader.hpp"
using namespace std;
using namespace chrono;

int N;
size_t M;
// the vertex every search starts from (see graph_helpers::search_source)
int source = 0;
// vertex u's neighbors are adj[offsets[u]] up to, not including, adj[offsets[u+1]]
graph_helpers::array<size_t> offsets;
graph_helpers::array<int> adj;
// expanded[u] is set once every neighbor of u has been examined
vector<int> dist, expanded;

int num_t;

// every access to a location that another thread may use at the same time,
// with one of them writing, is an atomic one. A store that lets other threads
// skip work, or that publishes what they read next, is a release store, after
// that work; a load whose value lets a thread skip work, or that it reads
// published data after, is an acquire load. The rest are relaxed
template <class T>
[[gnu::always_inline]] inline T shared_load(const T& object) {
    return atomic_ref<T>(const_cast<T&>(object)).load(memory_order_relaxed);
}

template <class T>
[[gnu::always_inline]] inline void shared_store(T& object, T value) {
    atomic_ref<T>(object).store(value, memory_order_relaxed);
}

template <class T>
[[gnu::always_inline]] inline T acquire_load(const T& object) {
    return atomic_ref<T>(const_cast<T&>(object)).load(memory_order_acquire);
}

template <class T>
[[gnu::always_inline]] inline void release_store(T& object, T value) {
    atomic_ref<T>(object).store(value, memory_order_release);
}
// the memory one thread hands out during a search, for its buckets' arrays or
// for the levels it adds, each kind from an arena of its own (see arenas). A
// block is handed out by moving a pointer; only a block that no chunk has room
// for takes a new chunk from the system allocator, which can take a lock, so a
// search that needs no more memory than the arena holds never waits on one.
// Once the search is over all of it is taken back at once rather than block by
// block. The chunks stay for later searches. Blocks start and end on 128-byte
// boundaries, so two that different threads write never share a line, nor the
// pair of lines that the L2 prefetcher fetches together. Arenas are padded the
// same way: before C++17 a vector of them is only 16-byte aligned, but two
// lines of padding still keep each arena's fields out of the next one's lines
class alignas(128) arena {
    public:
    static constexpr size_t block_alignment = 128;

    arena() = default;
    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;
    ~arena() {
        for(chunk& c: chunks) ::operator delete(c.begin);
    }

    void* allocate(size_t bytes) {
        bytes = (bytes + block_alignment - 1) & ~(block_alignment - 1);
        uintptr_t start = align_up(next);
        if(next == nullptr || start + bytes > reinterpret_cast<uintptr_t>(end)) {
            start = take_chunk(bytes);
        }
        next = reinterpret_cast<char*>(start + bytes);
        return reinterpret_cast<void*>(start);
    }

    // only once no thread can still read a block it handed out
    void reset() {
        current = 0;
        next = chunks.empty() ? nullptr : chunks[0].begin;
        end = chunks.empty() ? nullptr : chunks[0].begin + chunks[0].bytes;
    }

    private:
    struct chunk {
        char* begin;
        size_t bytes;
    };
    static constexpr size_t chunk_bytes = size_t(4) << 20;

    char* next = nullptr;
    char* end = nullptr;
    size_t current = 0;
    vector<chunk> chunks;

    static uintptr_t align_up(const char* p) {
        uintptr_t address = reinterpret_cast<uintptr_t>(p);
        return (address + block_alignment - 1) & ~(block_alignment - 1);
    }

    // the first chunk after the one in use that the block fits in, or a new one
    uintptr_t take_chunk(size_t bytes) {
        if(next != nullptr) current++;
        while(current < chunks.size() && chunks[current].bytes < bytes + block_alignment) {
            current++;
        }
        if(current == chunks.size()) {
            size_t size = max(chunk_bytes, bytes + block_alignment);
            chunks.push_back({static_cast<char*>(::operator new(size)), size});
        }
        next = chunks[current].begin;
        end = next + chunks[current].bytes;
        return align_up(next);
    }
};

// a thread's bucket of a level; it sits in the thread's entry of the level, which
// keeps it off the lines of other threads' buckets (see owner_entry)
class customList {
    public:
    int* list = nullptr;
    int size = 0;
    int capacity = 0;
    // the arena of the bucket's owner, the one thread that pushes to it
    arena* memory;

    explicit customList(arena* memory) : memory(memory) {}

    // readers load size before list, so publish the array contents, then list,
    // then size; otherwise a reader can index past what it is able to see
    [[gnu::always_inline]] inline void push_back(int x) {
        int s = shared_load(size);
        if(s == capacity) grow();
        shared_store(shared_load(list)[s], x);
        release_store(size, s + 1);
    }

    // an array smaller than an arena block would leave the rest of it unused
    [[gnu::noinline]] void grow() {
        reserve(max(capacity * 2, static_cast<int>(arena::block_alignment / sizeof(int))));
    }

    // a reader may still hold the old array, which stays in the arena until the
    // search is over. A new array at least doubles the capacity, even when a
    // caller reserves just past it, so the old ones together hold fewer
    // positions than the current one
    void reserve(int wanted) {
        if(wanted <= capacity) return;
        wanted = max(wanted, capacity * 2);
        int* new_list = static_cast<int*>(
            memory->allocate(sizeof(int) * static_cast<size_t>(wanted)));
        // helpers may be reading the old array through atomic_ref while it is
        // copied, so the copy reads it, and its size, the same way
        const int* old_list = shared_load(list);
        int copied = shared_load(size);
        for(int i=0; i<copied; i++) new_list[i] = shared_load(old_list[i]);
        // the copy is made of plain stores, which a thread that loads the new
        // array with an acquire load sees
        release_store(list, new_list);
        capacity = wanted;
    }
};

// one thread's entries in a level. The first line holds what threads read as
// they walk the level: its bucket's array and size, and the flag, set once.
// Only the owner changes the bucket, nearly always while the level before is
// expanded, but an owner stalled in that level can still push to it, and grow
// it, after other threads have started or finished this one. Such late entries
// are vertices the threads that finished the level before pushed too, so
// readers that miss them miss nothing. The owner rewrites its progress after
// every entry it finishes, so that goes on the second line, where those writes
// don't take the first from the readers. Each entry fills its own pair of
// lines, which the L2 prefetcher fetches together, so no two threads' entries
// share one
struct alignas(128) owner_entry {
    // only the owner pushes to it
    customList bucket;
    // set by the thread that finishes the bucket
    uint8_t done = 0;
    // how far the owner has got: the last entry of its bucket it finished
    alignas(64) int last_done = -1;

    explicit owner_entry(arena* memory) : bucket(memory) {}
};
static_assert(sizeof(owner_entry) == 128, "an entry is a pair of lines");

// a level's per-thread entries are an array of num_t, built by new_level
class outer_list_node {
    public:
    int depth;
    owner_entry* owners;
    atomic<outer_list_node*> next;
    outer_list_node() {
        next = nullptr;
    }
};

// bucket t's arrays come from arenas[t], since only thread t pushes to it, and
// nothing else does, so that memory only ever holds ints: had it held a level,
// C++ would need the ints' lifetimes started again before an array could reuse
// it. The levels a thread adds come from its entry in node_arenas
vector<arena> arenas;
vector<arena> node_arenas;

// a level, in the node arena of the thread that adds it. Arena blocks start on
// 128-byte boundaries, so every entry fills a pair of lines
outer_list_node* new_level(int depth, arena& memory) {
    outer_list_node* node = new(memory.allocate(sizeof(outer_list_node))) outer_list_node;
    node->depth = depth;
    node->owners = static_cast<owner_entry*>(memory.allocate(sizeof(owner_entry) * num_t));
    for(int t=0; t<num_t; t++) {
        new(&node->owners[t]) owner_entry(&arenas[t]);
    }
    return node;
}

struct affinePermutation {
    size_t index;
    size_t step;
};

size_t greatest_common_divisor(
        size_t first, size_t second) {
    while(second != 0) {
        size_t remainder = first % second;
        first = second;
        second = remainder;
    }
    return first;
}

affinePermutation affine_shuffle(
        size_t size, mt19937& rng) {
    if(size <= 1) return {0, 0};

    size_t index = static_cast<size_t>(rng()) % size;
    size_t step = 1 + static_cast<size_t>(rng()) % (size - 1);

    while(greatest_common_divisor(step, size) != 1) {
        step++;
        if(step == size) step = 1;
    }

    return {index, step};
}

[[gnu::always_inline]] inline size_t next_affine_index(
        size_t index, size_t step, size_t size) {
    index += step;
    return index >= size ? index - size : index;
}

void control_perf(const char* command) {
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

// push before marking: if this thread stops in between, v stays unmarked and
// another thread can still discover and push it. Every thread that marks v
// writes the same distance, so a store does, with no compare-and-swap
[[gnu::always_inline]] inline void discover_vertex(
        int v, customList& output_bucket, int next_distance) {
    output_bucket.push_back(v);
    release_store(dist[v], next_distance);
}

[[gnu::always_inline]] inline long long degree(int v) {
    return offsets[v+1] - offsets[v];
}

void process_neighbors(
        int u, customList& output_bucket, int next_distance) {
    const int* neighbors = adj.data() + offsets[u];
    size_t count = offsets[u+1] - offsets[u];
    for(size_t j=0; j<count; j++) {
        int v = neighbors[j];
        if(acquire_load(dist[v]) == -1) {
            discover_vertex(v, output_bucket, next_distance);
        }
    }
}

// helpers walk lists of up to three neighbors in order, as owners do: a thread
// is on one only briefly, and shuffling it would cost two random draws
constexpr size_t ordered_list_max = 3;

// a helper walks a long list as it walks a bucket: in a random order of its
// own. An owner and helpers that expand the same hub then write different
// entries of dist at any moment; walking the list in the same order, they
// would all write the same ones, each write waiting for the line
void process_neighbors_shuffled(
        int u, customList& output_bucket, int next_distance, mt19937& rng) {
    const int* neighbors = adj.data() + offsets[u];
    size_t count = offsets[u+1] - offsets[u];
    if(count <= ordered_list_max) {
        process_neighbors(u, output_bucket, next_distance);
        return;
    }
    affinePermutation order = affine_shuffle(count, rng);
    size_t j = order.index;
    for(size_t i=0; i<count; i++) {
        int v = neighbors[j];
        if(acquire_load(dist[v]) == -1) {
            discover_vertex(v, output_bucket, next_distance);
        }
        j = next_affine_index(j, order.step, count);
    }
}

// a list of more than hub_chunk neighbors is expanded in chunks of that many,
// each certified once it is done, so that the threads on a hub share its list
// rather than each scanning all of it. The certificate is the number of the
// search, written after the chunk's discoveries: a thread skips a chunk
// certified in this search, and one that stops leaves only the chunk it is on
// to be scanned again
constexpr size_t hub_chunk = 256;
vector<int> chunk_done;
thread_local int chunk_stamp = 0;

[[gnu::always_inline]] inline bool chunked(int u) {
    return static_cast<size_t>(degree(u)) > hub_chunk;
}

[[gnu::always_inline]] inline size_t chunk_count(int u) {
    return (static_cast<size_t>(degree(u)) + hub_chunk - 1) / hub_chunk;
}

// the certificate of u's chunk k is chunk_done[first_chunk(u) + k]; adding u
// keeps apart the chunks of two lists that meet inside one span of hub_chunk
// entries of adj
[[gnu::always_inline]] inline size_t first_chunk(int u) {
    return offsets[u] / hub_chunk + static_cast<size_t>(u);
}

// the neighbors [begin, end) of u's list, in order, or with rng in a random
// order, as process_neighbors_shuffled walks a whole list: threads that scan
// one chunk at once then write different entries of dist rather than each
// waiting for the same line
[[gnu::always_inline]] inline void process_neighbor_range(
        int u, size_t begin, size_t end, customList& output_bucket,
        int next_distance, mt19937* rng) {
    const int* neighbors = adj.data() + offsets[u];
    const size_t count = end - begin;
    if(rng == nullptr || count < 2) {
        for(size_t j=begin; j<end; j++) {
            int v = neighbors[j];
            if(acquire_load(dist[v]) == -1) {
                discover_vertex(v, output_bucket, next_distance);
            }
        }
        return;
    }
    affinePermutation order = affine_shuffle(count, *rng);
    size_t j = order.index;
    for(size_t i=0; i<count; i++) {
        int v = neighbors[begin + j];
        if(acquire_load(dist[v]) == -1) {
            discover_vertex(v, output_bucket, next_distance);
        }
        j = next_affine_index(j, order.step, count);
    }
}

// the chunks of u from start on in steps of step, an owner's in order from a
// chunk of its own and a helper's, given rng, in a random order, then u's mark:
// by then every chunk is certified, by this thread or another
void expand_chunked(
        int u, customList& output_bucket, int next_distance,
        size_t start, size_t step, mt19937* rng) {
    const size_t count = static_cast<size_t>(degree(u));
    const size_t chunks = chunk_count(u);
    int* certificates = chunk_done.data() + first_chunk(u);
    size_t k = start;
    for(size_t c=0; c<chunks; c++) {
        if(acquire_load(certificates[k]) != chunk_stamp) {
            const size_t begin = k * hub_chunk;
            process_neighbor_range(u, begin, min(begin + hub_chunk, count), output_bucket,
                                   next_distance, rng);
            // others skip a certified chunk, so its neighbors must be pushed first
            release_store(certificates[k], chunk_stamp);
        }
        k = next_affine_index(k, step, chunks);
    }
    // others skip a marked vertex, so every chunk must be certified first
    release_store(expanded[u], 1);
}

void process_own_bucket(
        outer_list_node* curr, outer_list_node* next_node,
        int tid, int sz
    ) {
    const int* bucket = shared_load(curr->owners[tid].bucket.list);
    customList& output_bucket = next_node->owners[tid].bucket;
    int& last_done = curr->owners[tid].last_done;

    for(int i=0; i<sz; i++) {
        int u = shared_load(bucket[i]);
        if(!acquire_load(expanded[u])) {
            int du = shared_load(dist[u]);
            if(chunked(u)) {
                // every thread that finds a hub undiscovered pushes it, so a hub can
                // be in many buckets, and owners that walked its chunks from the
                // same one would scan the same chunks at the same time
                const size_t start = static_cast<size_t>(tid) * chunk_count(u) / num_t;
                expand_chunked(u, output_bucket, du+1, start, 1, nullptr);
            }
            else {
                process_neighbors(u, output_bucket, du+1);
                // others skip a marked vertex, so its neighbors must be pushed first
                release_store(expanded[u], 1);
            }
        }
        // helpers skip entries up to here, so the entry must be done first
        release_store(last_done, i);
    }
}

void process_other_bucket(
        outer_list_node* curr, outer_list_node* next_node,
        int work_on, int tid, int sz, mt19937& rng
    ) {
    int start = acquire_load(curr->owners[work_on].last_done) + 1;
    if(start >= sz) return;
    size_t remaining = static_cast<size_t>(sz - start);

    // what is left, in a random order of this helper's own, so that helpers
    // that reach a small bucket together do not walk the same entries in step
    affinePermutation order = affine_shuffle(remaining, rng);

    const int* bucket = acquire_load(curr->owners[work_on].bucket.list) + start;
    customList& output_bucket = next_node->owners[tid].bucket;

    size_t i = order.index;
    for(size_t k=0; k<remaining; k++, i=next_affine_index(i, order.step, remaining)) {
        int u = shared_load(bucket[i]);
        if(acquire_load(expanded[u])) continue;

        int du = shared_load(dist[u]);
        if(chunked(u)) {
            affinePermutation chunk_order = affine_shuffle(chunk_count(u), rng);
            expand_chunked(u, output_bucket, du+1, chunk_order.index, chunk_order.step, &rng);
            continue;
        }
        process_neighbors_shuffled(u, output_bucket, du+1, rng);
        // others skip a marked vertex, so its neighbors must be pushed first
        release_store(expanded[u], 1);
    }
}

// each thread's row of num_t owners: itself, then the others on its NUMA node,
// then the rest, each group in round-robin order from it, so its helping
// reaches the other node only once its own node's owners are done. On one
// socket every other owner is in the first group
vector<int> help_order;
// per thread, how many of the others in its row are on its node
vector<int> near_owners;

void build_help_order(const vector<int>& cpu_ids) {
    map<int, int> node_of_cpu = cpu_helpers::detail::read_numa_nodes();
    vector<int> node(num_t);
    for(int t=0; t<num_t; t++) {
        auto found = node_of_cpu.find(cpu_ids[t]);
        node[t] = found == node_of_cpu.end() ? 0 : found->second;
    }
    help_order.assign(static_cast<size_t>(num_t) * num_t, 0);
    near_owners.assign(num_t, 0);
    for(int t=0; t<num_t; t++) {
        int* row = help_order.data() + static_cast<size_t>(t) * num_t;
        int n = 0;
        row[n++] = t;
        for(int k=1; k<num_t; k++) {
            if(node[(t + k) % num_t] == node[t]) row[n++] = (t + k) % num_t;
        }
        near_owners[t] = n - 1;
        for(int k=1; k<num_t; k++) {
            if(node[(t + k) % num_t] != node[t]) row[n++] = (t + k) % num_t;
        }
    }
}

// the owners a thread visits in a level: itself, then each group of its row of
// help_order in a random order drawn for every walk. In a fixed order, threads
// that run out of work at about the same time walk the same buckets together
// and expand the same entries. owner(k) must be called for k = 0, 1, ... in
// turn, or from 1 on to leave the thread itself out
struct ownerWalk {
    int tid;
    int near;
    affinePermutation near_order;
    affinePermutation far_order;

    ownerWalk(int thread, mt19937& rng) : tid(thread), near(near_owners[thread]) {
        near_order = affine_shuffle(static_cast<size_t>(near), rng);
        far_order = affine_shuffle(static_cast<size_t>(num_t - 1 - near), rng);
    }

    int owner(int k) {
        int position = k;
        if(k > near) {
            position = 1 + near + static_cast<int>(far_order.index);
            far_order.index = next_affine_index(far_order.index, far_order.step, num_t - 1 - near);
        }
        else if(k > 0) {
            position = 1 + static_cast<int>(near_order.index);
            near_order.index = next_affine_index(near_order.index, near_order.step, near);
        }
        return help_order[static_cast<size_t>(tid) * num_t + position];
    }
};

// the level this thread built but lost the race to link, if any (see add_node)
thread_local outer_list_node* spare_level = nullptr;

// threads reach a level together, so nearly all of them build its successor and
// all but one lose the race to link it. A level that loses was never seen by
// another thread, so it is kept for the next one this thread adds, and only its
// depth changes; otherwise every loser would stay in its thread's arena until
// the search is over
void add_node(outer_list_node* curr, int tid) {
    outer_list_node* new_node = spare_level;
    if(new_node == nullptr) {
        new_node = new_level(curr->depth + 1, node_arenas[tid]);
    }
    else {
        new_node->depth = curr->depth + 1;
    }
    outer_list_node* expected = nullptr;
    bool linked = atomic_compare_exchange_strong(&(curr->next), &expected, new_node);
    spare_level = linked ? nullptr : new_node;
}

void top_down_level(
        outer_list_node* curr, int tid, mt19937& rng) {
    ownerWalk walk(tid, rng);
    bool next_is_null = true;

    // the first owner is this thread itself
    for(int k=0; k<num_t; k++) {
        int work_on = walk.owner(k);
        if(acquire_load(curr->owners[work_on].done) ||
                acquire_load(curr->owners[work_on].bucket.size) == 0) {
            continue;
        }

        if(next_is_null && curr->next == nullptr) {
            add_node(curr, tid);
        }
        next_is_null = false;

        outer_list_node* next_node = curr->next.load();

        // the vertices up to this size, and the array that holds them, are seen
        // after it (see push_back)
        int sz = acquire_load(curr->owners[work_on].bucket.size);

        if(work_on == tid) {
            process_own_bucket(curr, next_node, tid, sz);
        }
        else {
            process_other_bucket(
                curr, next_node, work_on, tid, sz, rng);
        }

        // the flag shares its line with the bucket, which every thread reads at every
        // level; rewriting a set flag would take the line from all of them for nothing
        if(!shared_load(curr->owners[work_on].done)) {
            release_store(curr->owners[work_on].done, uint8_t(1));
        }
    }
}

// a level that finds nothing links no successor, which ends the search
void wf_bfs(outer_list_node* head, int tid, mt19937& rng) {
    for(outer_list_node* curr = head; curr != nullptr; curr = curr->next) {
        // what this thread wrote in the level before may still be in its store
        // buffer, where only it sees it, and the first thread to read this
        // level's buckets must have made all of that visible to the others
        atomic_thread_fence(memory_order_seq_cst);
        top_down_level(curr, tid, rng);
    }
}

void init(int N) {
    dist.resize(N, -1);
    expanded.resize(N, 0);
}

// fills each vertex's neighbors in file order, so the lists match wf-dir.cpp's
void load_graph(const string& path) {
    graph_helpers::edge_list edges = graph_helpers::read_graph(path);
    N = edges.n;
    M = edges.from.size();
    init(N);
    graph_helpers::build_csr(edges, offsets, adj);
    // first_chunk(u) + k stays below this for every chunk k of every u
    chunk_done.assign(static_cast<size_t>(N) + offsets[N] / hub_chunk + 1, 0);
}

// a search starts from the source, the one entry of its first level. The threads
// are idle while it is set up, so it can come from thread 0's arenas
outer_list_node* make_first_level() {
    outer_list_node* head = new_level(0, node_arenas[0]);
    head->owners[0].bucket.push_back(source);
    return head;
}

// only once every thread has returned, since until then one may still be
// reading a bucket
void free_levels() {
    for(int t=0; t<num_t; t++) {
        arenas[t].reset();
        node_arenas[t].reset();
    }
}

// thread tid's share of what a search starts from: a slice of dist and expanded,
// with the source at 0, and no spare level, since the arenas took the last one
// back
void reset_share(int tid) {
    spare_level = nullptr;
    int begin = static_cast<int>(1LL * N * tid / num_t);
    int end = static_cast<int>(1LL * N * (tid + 1) / num_t);
    fill(dist.begin() + begin, dist.begin() + end, -1);
    fill(expanded.begin() + begin, expanded.begin() + end, 0);
    if(begin <= source && source < end) dist[source] = 0;
}

// the threads live for the whole run, as GAPBS's OpenMP threads do, so no
// search pays for starting them. The main thread sets up each search and
// publishes its number; every thread then resets its share, searches, and
// notes when it returned
struct search_control {
    outer_list_node* head = nullptr;
    // the search to run, numbered from 1; -1 stops the threads
    atomic<int> number{0};
    // shares reset and searches returned, counted over all searches so far
    atomic<int> reset{0};
    atomic<int> returned{0};
    mutex returned_mutex;
    condition_variable all_returned;
    vector<high_resolution_clock::time_point> return_times;
};
search_control searches;

void search_thread(int tid, int cpu) {
    cpu_helpers::pin_current_thread(cpu);
    mt19937 rng(chrono::steady_clock::now().time_since_epoch().count());

    for(int last = 0; ; ) {
        int number;
        while((number = searches.number.load()) == last) __builtin_ia32_pause();
        if(number < 0) return;
        last = number;
        chunk_stamp = number;

        reset_share(tid);
        // a thread still resetting could overwrite a distance another has
        // already set, so no thread searches before every share is reset
        searches.reset++;
        while(searches.reset.load() < number * num_t) __builtin_ia32_pause();

        wf_bfs(searches.head, tid, rng);
        searches.return_times[tid] = high_resolution_clock::now();
        if(++searches.returned == number * num_t) {
            lock_guard<mutex> lock(searches.returned_mutex);
            searches.all_returned.notify_one();
        }
    }
}

void write_output() {
    graph_helpers::write_distances("wfd-out.txt", dist);
}

// the timed searches on num_t threads pinned to cpu_ids, returning their
// average in microseconds. All that is sized by the thread count, and the
// counts the threads meet at, start afresh, so one thread count can follow
// another
long long run_searches(const vector<int>& cpu_ids) {
    build_help_order(cpu_ids);
    arenas = vector<arena>(num_t);
    node_arenas = vector<arena>(num_t);
    // the searches of every thread count are numbered from 1
    fill(chunk_done.begin(), chunk_done.end(), 0);
    searches.return_times.assign(num_t, {});
    searches.number = 0;
    searches.reset = 0;
    searches.returned = 0;
    vector<thread> threads;
    for(int i=0; i<num_t; i++) {
        threads.emplace_back(search_thread, i, cpu_ids[i]);
    }

    constexpr int repetitions = 20;
    long long total_duration = 0;

    for(int i=1; i<=repetitions; i++) {
        // counted one search at a time, since in between the threads only spin
        control_perf("enable");
        high_resolution_clock::time_point t1 = high_resolution_clock::now();
        searches.head = make_first_level();
        searches.number = i;
        {
            unique_lock<mutex> lock(searches.returned_mutex);
            searches.all_returned.wait(
                lock, [&] { return searches.returned.load() == i * num_t; });
        }
        control_perf("disable");
        // a search ends when its first thread returns: a thread returns only
        // once every level is done, so the distances are final by then, and
        // the threads still running can only write the same ones again
        high_resolution_clock::time_point t2 = *min_element(
            searches.return_times.begin(), searches.return_times.end());

        total_duration += duration_cast<microseconds>(t2 - t1).count();
        free_levels();
    }

    searches.number = -1;
    for(thread& t: threads) t.join();

    return total_duration / repetitions;
}

int main(int argc, char *argv[]) {
    ios_base::sync_with_stdio(false); cin.tie(0); cout.tie(0);

    if(argc != 3) {
        cout << "Usage: " << argv[0] << " <input_file> <num_threads>[,<num_threads>...]\n";
        return 1;
    }

    try {
        load_graph(argv[1]);
        source = graph_helpers::search_source(static_cast<size_t>(N));
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

        average_duration = run_searches(cpu_ids);
        if(thread_counts.size() > 1) {
            if(c == 0) first_dist = dist;
            cout << num_t << " " << average_duration << " "
                 << (dist == first_dist ? "same" : "differs") << endl;
        }
    }

    write_output();

    if(thread_counts.size() == 1) cout << average_duration << "\n";

    return 0;
}

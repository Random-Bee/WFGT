// WF_DO_BFS/wf-dir-mem.cpp with the ordering of the builds that the paper
// times: relaxed accesses, kept in order by compiler barriers, which rely on
// GCC and on x86 keeping stores in order and loads in order, rather than
// release stores and acquire loads: mem in ordering-times.sh and copy-times.sh
#include <bits/stdc++.h>
#include "../../cpu_helpers/cpu_affinity.hpp"
#include "../../graph_helpers/graph_reader.hpp"
using namespace std;
using namespace chrono;

int N;
size_t M;
// the vertex every search starts from (see graph_helpers::search_source)
int source = 0;
// vertex u's neighbors are adj[offsets[u]] up to, not including, adj[offsets[u+1]]
graph_helpers::array<size_t> offsets;
graph_helpers::array<int> adj;
// expanded[u] is set once a top-down level has examined every neighbor of u
vector<int> dist, expanded;
// a one-byte code per vertex for the bottom-up frontier test, a quarter the size
// of dist. Each level that a bottom-up level reads or writes gets its own code
// from the level plan, handed out in increasing order and never reused within a
// search, so a code names one level at any depth and a stale thread's test stays
// exact. 0 is no code; once max_code is used up, levels test dist. Bottom-up
// discoveries write their code before dist, so a thread that sees a vertex
// marked also sees its code; top-down pushes write none, since many threads
// pushing nearby vertices fight over the lines, and the first bottom-up level
// after them writes its frontier's codes before testing
constexpr int max_code = 255;
vector<uint8_t> dist8;

int num_t;

// keeps the compiler from moving memory accesses across this point; x86 itself
// keeps stores in order and loads in order (SDM Vol. 3A, 9.2.2, December 2022)
[[gnu::always_inline]] inline void compiler_barrier() {
    asm volatile("" ::: "memory");
}

// every access to a location that another thread may use at the same time,
// with one of them writing, is an atomic one, relaxed unless noted; the
// barriers keep their order
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

// the memory one thread hands out during a search, for its buckets' arrays or
// for the records it proposes, each kind from an arena of its own (see arenas).
// A block is handed out by moving a pointer; only a block that no chunk has
// room for takes a new chunk from the system allocator, which can take a lock,
// so a search that needs no more memory than the arena holds never waits on
// one. Once the search is over all of it is taken back at once rather than
// block by block. The chunks stay for later searches. Blocks start and end on
// 128-byte boundaries, so two that different threads write never share a line,
// nor the pair of lines that the L2 prefetcher fetches together. Arenas are
// padded the same way: before C++17 a vector of them is only 16-byte aligned,
// but two lines of padding still keep each arena's fields out of the next
// one's lines
class alignas(128) arena {
    public:
    static constexpr size_t block_alignment = 128;

    arena() = default;
    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;
    ~arena() {
        for(chunk& c: chunks) std::free(c.begin);
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
            // zeroed: a slow reader of a reused bucket can read entries that no
            // thread has written in this search (see at_level), and C++ leaves
            // memory that was never written without a value to read
            char* memory = static_cast<char*>(std::calloc(size, 1));
            if(memory == nullptr) throw std::bad_alloc();
            chunks.push_back({memory, size});
        }
        next = chunks[current].begin;
        end = next + chunks[current].bytes;
        return align_up(next);
    }
};

// a thread's buckets' arrays come from its entry here, and nothing else does,
// so this memory only ever holds ints: zero from calloc, or vertices written
// since. Had it held a record, C++ would need the ints' lifetimes started
// again, with values, before a slow reader of a reused bucket could read them
// (see at_level). The records a thread proposes come from record_arenas
vector<arena> arenas;
vector<arena> record_arenas;

// each thread rewrites its size and degree sum on every push, so no two buckets
// may share a cache line. Before C++17 a vector aligns its storage to only 16
// bytes, so one line of padding can leave a bucket's degree sum in the line of
// the next bucket's size; two lines keep them apart at any 16-byte alignment
class alignas(128) customList {
    public:
    int* list = nullptr;
    int size = 0;
    int capacity = 0;
    // the level these entries belong to; a node's buckets are reused two levels
    // on, so readers ignore a bucket that holds another level
    int level = -1;
    // the latest level at which the owner's share of the level, and the writing
    // of its frontier's codes, are known to be finished. Each is set about once
    // a level, on the line that threads walking the level read anyway; a slow
    // thread may lower one again, which only makes others check work that is done
    int done = -1;
    int codes_done = -1;
    // summed degrees of the vertices pushed: the direction rule compares the
    // top-down sums with the edges still unexplored, and a bottom-up level
    // sizes its buckets by them
    long long degree_sum = 0;
    // the arena of the bucket's owner, the one thread that pushes to it
    arena* memory = nullptr;

    // only while no thread is searching: the arena takes the last search's
    // arrays back, so the bucket starts without one
    void begin_search(arena* owner_memory) {
        list = nullptr;
        size = 0;
        capacity = 0;
        level = -1;
        done = -1;
        codes_done = -1;
        degree_sum = 0;
        memory = owner_memory;
    }

    // only the owner calls this, before its first push of the level; the array
    // is kept, and entries of the level two back are overwritten
    void begin_level(int new_level) {
        shared_store(size, 0);
        shared_store(degree_sum, 0LL);
        // readers check the level before reading the size
        compiler_barrier();
        shared_store(level, new_level);
    }

    // readers load size before list, so publish the array contents, then list,
    // then size; otherwise a reader can index past what it is able to see
    [[gnu::always_inline]] inline void push_back(int x) {
        int s = shared_load(size);
        if(s == capacity) grow();
        shared_store(shared_load(list)[s], x);
        compiler_barrier();
        shared_store(size, s + 1);
    }

    // an array smaller than an arena block would leave the rest of it unused
    [[gnu::noinline]] void grow() {
        reserve(max(capacity * 2, static_cast<int>(arena::block_alignment / sizeof(int))));
    }

    // a reader may still hold the old array, which stays in the arena until the
    // search is over. A new array at least doubles the capacity, even when a
    // caller reserves just past it, so the old ones together hold fewer
    // positions than the current one. Past the entries copied, the new array
    // holds what the arena's memory held, zero or a vertex of an earlier level
    // or search, which a slow reader of a reused bucket can meet
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
        // the copy is made of plain stores, so a thread that loads the new array
        // from another thread's bucket makes an acquire fence before reading it
        atomic_thread_fence(memory_order_release);
        shared_store(list, new_list);
        capacity = wanted;
    }
};

// padded to a cache line, since each owner rewrites its entry as it goes
struct alignas(64) owner_progress {
    int last_done;
};

enum level_direction { top_down, bottom_up };

// what the direction rule carries from one level to the next; the thread whose
// record decides a level works it out, and every thread takes it from there
struct level_plan {
    level_direction direction;
    long long edges_to_check;
    long long frontier_size;
    // vertices in no frontier yet; a vertex pushed twice is counted twice, so
    // this can run below the true count
    long long undiscovered;
    // the code of this level's frontier in dist8, or 0, and the highest code
    // handed out so far
    int code;
    int last_code;
};

// a level's decision: the plan that every thread follows in it, and the slot
// where the next level's decision goes. A thread that finds a level undecided
// fills in a record of its own and tries to install it in the level's slot with
// a CAS from null, and the first to succeed decides for all. Nothing changes a
// record once it is installed, so a thread that reads one finds it whole, and
// records stay until the search ends, so a thread that has fallen behind can
// still read the plan of every level. Each thread's own part of a level lives in
// the two reused nodes below. The slot is on a pair of lines of its own, so that
// the CAS that later decides the next level takes no line that readers of this
// plan need
struct level_record {
    level_plan plan;
    alignas(128) atomic<level_record*> next{nullptr};

    // an empty level ends the search, and no level after it is ever decided
    bool ends_search() const {
        return plan.frontier_size == 0;
    }

    // the next level is decided only by a thread that has finished this one, so
    // this level is over once it is. Threads test this between the owners of a
    // level, as WF-DO's level_done, so that they can leave a level that another
    // thread has finished
    bool finished() const {
        return next.load() != nullptr;
    }
};

// the first level's slot, which start_search empties
alignas(128) atomic<level_record*> first_slot{nullptr};

// each thread's part of one level. There are two nodes, one for the even levels
// and one for the odd: a thread reuses its part of a node for level L+2 only
// once it is past level L, which is over by then, so no work is left in what it
// reuses. The buckets carry the level they belong to, so a thread that has
// fallen behind can tell when another thread's part has moved on
class outer_list_node {
    public:
    // with each owner's done flags (see customList)
    vector<customList> buckets;
    // how far each owner has got on the node's level: the last entry of its
    // bucket it finished top-down, the last of its blocks bottom-up (see last_done_on)
    vector<owner_progress> progress;
    // the same for writing the frontier's codes, on the first bottom-up level
    // after top-down ones (see codes_done_on)
    vector<owner_progress> codes_progress;
    outer_list_node() : buckets(num_t), progress(num_t), codes_progress(num_t) {}

    // only while no thread is searching
    void begin_search() {
        for(int t=0; t<num_t; t++) {
            buckets[t].begin_search(&arenas[t]);
            progress[t].last_done = -1;
            codes_progress[t].last_done = -1;
        }
    }

    void mark_done(int owner, int level) {
        int& done = buckets[owner].done;
        if(shared_load(done) < level) shared_store(done, level);
    }

    void mark_codes_done(int owner, int level) {
        int& done = buckets[owner].codes_done;
        if(shared_load(done) < level) shared_store(done, level);
    }
};

// the last entry or block the owner finished on this level, -1 if it hasn't
// started the level. An owner resets its progress before it tags its next
// bucket with the next level, so the progress counts only once that tag is in
// place. A position from a later level is safe as well, since an owner leaves
// a level only once its share of it is done
[[gnu::always_inline]] inline int last_done_on(
        outer_list_node* curr, outer_list_node* next_node, int owner, int level) {
    if(shared_load(next_node->buckets[owner].level) != level + 1) return -1;
    compiler_barrier();
    return shared_load(curr->progress[owner].last_done);
}

// the last entry of its bucket whose code the owner wrote on this level, by the
// same reasoning as last_done_on
[[gnu::always_inline]] inline int codes_done_on(
        outer_list_node* curr, outer_list_node* next_node, int owner, int level) {
    if(shared_load(next_node->buckets[owner].level) != level + 1) return -1;
    compiler_barrier();
    return shared_load(curr->codes_progress[owner].last_done);
}

// a vertex id read from another owner's bucket: the owner may already have
// reused the bucket for the level two on, and a slow reader can then meet that
// level's entries, or past what the owner rewrote, memory never written in this
// search. Only an id in range whose dist is the reader's level belongs to it
[[gnu::always_inline]] inline bool at_level(int u, int level) {
    return static_cast<unsigned>(u) < static_cast<unsigned>(N) && shared_load(dist[u]) == level;
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
// another thread can still discover and push it. The code goes before the mark
// too, so a thread that finds v marked also finds its code; past the last code
// it writes 0, which an undiscovered v already has
[[gnu::always_inline]] inline void discover_vertex(
        int v, customList& output_bucket, int next_distance, uint8_t next_code) {
    output_bucket.push_back(v);
    compiler_barrier();
    shared_store(dist8[v], next_code);
    compiler_barrier();
    shared_store(dist[v], next_distance);
}

[[gnu::always_inline]] inline long long degree(int v) {
    return offsets[v+1] - offsets[v];
}

// the owner's running sum, which planners read at the same time
[[gnu::always_inline]] inline void add_degrees(customList& bucket, long long degrees) {
    shared_store(bucket.degree_sum, shared_load(bucket.degree_sum) + degrees);
}

// pushed before marking, as in discover_vertex. Of the threads that push v,
// only the one whose swap marks it counts its edges: the direction rule
// subtracts them from those still unexplored, and counting copies runs that
// total down long before the search ends
[[gnu::always_inline]] inline void discover_and_count(
        int v, customList& output_bucket, int next_distance) {
    output_bucket.push_back(v);
    int expected = -1;
    if(atomic_ref<int>(dist[v]).compare_exchange_strong(expected, next_distance)) {
        add_degrees(output_bucket, degree(v));
    }
}

void process_neighbors(
        int u, customList& output_bucket, int next_distance) {
    const int* neighbors = adj.data() + offsets[u];
    size_t count = offsets[u+1] - offsets[u];
    for(size_t j=0; j<count; j++) {
        int v = neighbors[j];
        if(shared_load(dist[v]) == -1) {
            discover_and_count(v, output_bucket, next_distance);
        }
    }
}

// helpers walk lists of up to three neighbors in order, as owners do: a thread
// is on one only briefly, and shuffling it would cost two random draws
constexpr size_t ordered_list_max = 3;

// a helper walks a long list as it walks a bucket: in a random order of its
// own. An owner and helpers that expand the same hub then swap different
// entries of dist at any moment; walking the list in the same order, they
// would all swap the same ones, each swap waiting for the line
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
        if(shared_load(dist[v]) == -1) {
            discover_and_count(v, output_bucket, next_distance);
        }
        j = next_affine_index(j, order.step, count);
    }
}

// a list of more than hub_chunk neighbors is expanded in chunks of that many,
// each certified once it is done, so that the threads on a hub share its list
// rather than each scanning all of it. The certificate is the number of the
// search, written after the chunk's discoveries: a thread skips a chunk
// certified in this search, and one that stops leaves only the chunk it is on
// to be scanned again. A vertex is expanded at one level of a search, so the
// reuse of nodes leaves the certificates alone
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
// one chunk at once then swap different entries of dist rather than each
// waiting for the same line
[[gnu::always_inline]] inline void process_neighbor_range(
        int u, size_t begin, size_t end, customList& output_bucket,
        int next_distance, mt19937* rng) {
    const int* neighbors = adj.data() + offsets[u];
    const size_t count = end - begin;
    if(rng == nullptr || count < 2) {
        for(size_t j=begin; j<end; j++) {
            int v = neighbors[j];
            if(shared_load(dist[v]) == -1) {
                discover_and_count(v, output_bucket, next_distance);
            }
        }
        return;
    }
    affinePermutation order = affine_shuffle(count, *rng);
    size_t j = order.index;
    for(size_t i=0; i<count; i++) {
        int v = neighbors[begin + j];
        if(shared_load(dist[v]) == -1) {
            discover_and_count(v, output_bucket, next_distance);
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
        if(shared_load(certificates[k]) != chunk_stamp) {
            const size_t begin = k * hub_chunk;
            process_neighbor_range(u, begin, min(begin + hub_chunk, count), output_bucket,
                                   next_distance, rng);
            // others skip a certified chunk, so its neighbors must be pushed first
            compiler_barrier();
            shared_store(certificates[k], chunk_stamp);
        }
        k = next_affine_index(k, step, chunks);
    }
    // others skip a marked vertex, so every chunk must be certified first
    compiler_barrier();
    shared_store(expanded[u], 1);
}

void process_own_bucket(
        outer_list_node* curr, outer_list_node* next_node,
        int level, int tid, int sz
    ) {
    const int* bucket = shared_load(curr->buckets[tid].list);
    customList& output_bucket = next_node->buckets[tid];
    int& last_done = curr->progress[tid].last_done;

    for(int i=0; i<sz; i++) {
        int u = shared_load(bucket[i]);
        if(!shared_load(expanded[u])) {
            if(chunked(u)) {
                // every thread that finds a hub undiscovered pushes it, so a hub can
                // be in many buckets, and owners that walked its chunks from the
                // same one would scan the same chunks at the same time
                const size_t start = static_cast<size_t>(tid) * chunk_count(u) / num_t;
                expand_chunked(u, output_bucket, level+1, start, 1, nullptr);
            }
            else
            {
                process_neighbors(u, output_bucket, level+1);
                // others skip a marked vertex, so its neighbors must be pushed first
                compiler_barrier();
                shared_store(expanded[u], 1);
            }
        }
        // helpers skip entries up to here, so the entry must be done first
        compiler_barrier();
        shared_store(last_done, i);
    }
}

// the owner may already have reused its bucket for the level two on (see at_level)
void process_other_bucket(
        outer_list_node* curr, outer_list_node* next_node,
        int level, int work_on, int tid, int sz, mt19937& rng
    ) {
    int start = last_done_on(curr, next_node, work_on, level) + 1;
    if(start >= sz) return;
    size_t remaining = static_cast<size_t>(sz - start);

    // what is left, in a random order of this helper's own, so that helpers
    // that reach a small bucket together do not walk the same entries in step
    affinePermutation order = affine_shuffle(remaining, rng);

    const int* bucket = shared_load(curr->buckets[work_on].list) + start;
    customList& output_bucket = next_node->buckets[tid];

    size_t i = order.index;
    // pairs with the release fence in customList::reserve
    atomic_thread_fence(memory_order_acquire);
    for(size_t k=0; k<remaining; k++, i=next_affine_index(i, order.step, remaining)) {
        int u = shared_load(bucket[i]);
        if(!at_level(u, level) || shared_load(expanded[u])) continue;

        if(chunked(u)) {
            affinePermutation chunk_order = affine_shuffle(chunk_count(u), rng);
            expand_chunked(u, output_bucket, level+1, chunk_order.index, chunk_order.step, &rng);
            continue;
        }
        process_neighbors_shuffled(u, output_bucket, level+1, rng);
        // others skip a marked vertex, so its neighbors must be pushed first
        compiler_barrier();
        shared_store(expanded[u], 1);
    }
}

struct vertexRange {
    int begin;
    int end;
};

// bottom-up levels hand out vertices in small blocks: block b belongs to thread
// b % num_t, so every thread's blocks sample the whole id range, and a level
// whose work sits in one part of that range still spreads evenly. As with
// top-down buckets, each owner publishes the last of its blocks it finished,
// and helpers take the ones after that. Blocks much smaller than this are
// slower, since each starts a new run through the neighbor lists that the
// hardware prefetcher has to pick up again
constexpr int bottom_up_block = 1024;
int block_count;

[[gnu::always_inline]] inline int owned_blocks(int t) {
    return t < block_count ? (block_count - 1 - t) / num_t + 1 : 0;
}

// the vertices of thread t's k-th block
[[gnu::always_inline]] inline vertexRange owned_block(int t, int k) {
    int begin = (t + k*num_t) * bottom_up_block;
    return {begin, min(begin + bottom_up_block, N)};
}

// the bottom-up level at which a helper last finished each block, so other
// helpers and a late owner skip it; without it, every helper walks all of a
// stalled owner's blocks. Laid out by owner, so an owner's entries share lines
// only with its helpers. A stale thread can only write an older level, which
// costs a repeated scan but never a skipped block
vector<int> helped_at;
int blocks_per_owner;

[[gnu::always_inline]] inline int& helped_at_level(int t, int k) {
    return helped_at[t * blocks_per_owner + k];
}

[[gnu::always_inline]] inline bool has_frontier_neighbor(
        const int* neighbors, size_t count, int depth) {
    for(size_t j=0; j<count; j++) {
        if(shared_load(dist[neighbors[j]]) == depth) return true;
    }
    return false;
}

// has_frontier_neighbor on dist8, for a level with a code
[[gnu::always_inline]] inline bool has_frontier_neighbor8(
        const int* neighbors, size_t count, uint8_t code) {
    for(size_t j=0; j<count; j++) {
        if(shared_load(dist8[neighbors[j]]) == code) return true;
    }
    return false;
}

// the neighbor test is inlined, since most tests stop after a few neighbors
// and the call around each was a large part of the cost
void scan_block(
        vertexRange range, int depth, int code, customList& output_bucket) {
    // the block's undiscovered vertices, gathered without branching on each:
    // every vertex is written, but the count moves only past undiscovered
    // ones; on some levels about half are, and a branch on it mispredicts often
    int candidates[bottom_up_block];
    int n = 0;
    for(int u=range.begin; u<range.end; u++) {
        candidates[n] = u;
        n += shared_load(dist[u]) == -1;
    }
    if(code > 0) {
        // what this level finds is the next level's frontier, under the next
        // code, which the plan reserved for it
        uint8_t frontier_code = static_cast<uint8_t>(code);
        uint8_t next_code = code < max_code ? static_cast<uint8_t>(code + 1) : 0;
        for(int i=0; i<n; i++) {
            int u = candidates[i];
            // read once: the barriers in discover_vertex would make the
            // compiler read the offsets again for the degree
            size_t begin = offsets[u], end = offsets[u+1];
            if(has_frontier_neighbor8(adj.data() + begin, end - begin, frontier_code)) {
                discover_vertex(u, output_bucket, depth + 1, next_code);
                add_degrees(output_bucket, static_cast<long long>(end - begin));
            }
        }
        return;
    }
    for(int i=0; i<n; i++) {
        int u = candidates[i];
        size_t begin = offsets[u], end = offsets[u+1];
        if(has_frontier_neighbor(adj.data() + begin, end - begin, depth)) {
            discover_vertex(u, output_bucket, depth + 1, 0);
            add_degrees(output_bucket, static_cast<long long>(end - begin));
        }
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

// own blocks, in order, publishing each as the last one done and skipping those
// a helper got to first; a helper that finishes the rest marks the owner done,
// which stops it
void fetch_own_blocks(
        outer_list_node* curr, int level, customList& output_bucket, int tid, int code) {
    int count = owned_blocks(tid);
    int& last_done = curr->progress[tid].last_done;
    for(int k=0; k<count && shared_load(curr->buckets[tid].done) < level; k++) {
        if(shared_load(helped_at_level(tid, k)) != level) {
            scan_block(owned_block(tid, k), level, code, output_bucket);
        }
        // helpers skip blocks up to here, so their vertices must be marked first
        compiler_barrier();
        shared_store(last_done, k);
    }
    curr->mark_done(tid, level);
}

// the blocks each other owner hasn't done yet, in a random order as top-down
// helpers take bucket entries, so helpers on the same owner spread out; blocks
// the owner or another helper has done since are skipped, and a helper that
// gets through the rest marks the owner done
void fetch_other_blocks(
        outer_list_node* curr, outer_list_node* next_node, const level_record& record,
        int level, customList& output_bucket, int tid, mt19937& rng, int code) {
    ownerWalk walk(tid, rng);
    for(int k=1; k<num_t; k++) {
        if(record.finished()) return;
        int owner = walk.owner(k);
        if(shared_load(curr->buckets[owner].done) >= level) continue;
        int start = last_done_on(curr, next_node, owner, level) + 1;
        int remaining = owned_blocks(owner) - start;
        affinePermutation order = affine_shuffle(max(remaining, 0), rng);
        size_t index = order.index;
        for(int i=0; i<remaining; i++) {
            // read again for every block, since the owner and other helpers go on
            compiler_barrier();
            if(shared_load(curr->buckets[owner].done) >= level) break;
            int block = start + static_cast<int>(index);
            if(block > last_done_on(curr, next_node, owner, level) &&
                    shared_load(helped_at_level(owner, block)) != level) {
                scan_block(owned_block(owner, block), level, code, output_bucket);
                // others skip the block once this is set, so its vertices must be marked first
                compiler_barrier();
                shared_store(helped_at_level(owner, block), level);
            }
            index = next_affine_index(index, order.step, remaining);
        }
        curr->mark_done(owner, level);
    }
}

void top_down_level(
        outer_list_node* curr, outer_list_node* next_node, const level_record& record,
        int level, int tid, mt19937& rng) {
    ownerWalk walk(tid, rng);

    for(int k=0; k<num_t; k++) {
        if(record.finished()) return;
        int work_on = walk.owner(k);
        customList& bucket = curr->buckets[work_on];
        // done already, or the bucket holds another level
        bool skip = shared_load(bucket.done) >= level || shared_load(bucket.level) != level;
        // the level before the size (see begin_level)
        compiler_barrier();
        if(skip || shared_load(bucket.size) == 0) {
            continue;
        }

        int sz = shared_load(bucket.size);
        // size must be read before the list pointer (see push_back)
        compiler_barrier();

        if(work_on == tid) {
            process_own_bucket(curr, next_node, level, tid, sz);
        }
        else {
            process_other_bucket(
                curr, next_node, level, work_on, tid, sz, rng);
        }

        curr->mark_done(work_on, level);
    }
}

// writes `code` for a frontier that top-down pushes left without one: this
// thread's own bucket in order, publishing how far it got, then whatever is left
// of each bucket no thread has finished. Every bucket is done when it returns,
// so the frontier test can start. A bucket that holds another level has none of
// this level's entries; in another owner's bucket only the ids at_level takes
// get the code. A code written twice or late is still right, since it goes only
// to vertices at the level
void write_frontier_codes(
        outer_list_node* curr, outer_list_node* next_node, const level_record& record,
        int level, int tid, uint8_t code, mt19937& rng) {
    ownerWalk walk(tid, rng);
    for(int k=0; k<num_t; k++) {
        // a level that is over has had all its codes written
        if(record.finished()) return;
        int owner = walk.owner(k);
        const customList& bucket = curr->buckets[owner];
        if(shared_load(bucket.codes_done) >= level) continue;
        bool this_level = shared_load(bucket.level) == level;
        // the level before the size (see begin_level)
        compiler_barrier();
        if(this_level) {
            int sz = shared_load(bucket.size);
            // size must be read before the list pointer (see push_back)
            compiler_barrier();
            // pairs with the release fence in customList::reserve
            const int* list = acquire_load(bucket.list);
            if(owner == tid) {
                int& progress = curr->codes_progress[tid].last_done;
                for(int i=0; i<sz; i++) {
                    shared_store(dist8[shared_load(list[i])], code);
                    // helpers start past here, so the code must be written first
                    compiler_barrier();
                    shared_store(progress, i);
                }
            }
            else {
                for(int i=codes_done_on(curr, next_node, owner, level)+1; i<sz; i++) {
                    int u = shared_load(list[i]);
                    if(at_level(u, level)) shared_store(dist8[u], code);
                }
            }
        }
        // others skip the bucket once this is set, so its codes must be written first
        compiler_barrier();
        curr->mark_codes_done(owner, level);
    }
}

void bottom_up_level(
        outer_list_node* curr, outer_list_node* next_node, const level_record& record,
        int level, int tid, mt19937& rng, long long undiscovered,
        level_direction previous, int code) {
    if(record.finished()) return;
    // after a top-down level the code is fresh, and nothing has written it yet
    if(previous == top_down && code > 0) {
        write_frontier_codes(curr, next_node, record, level, tid, static_cast<uint8_t>(code), rng);
    }
    // again before this thread's own blocks, and the space it reserves for them
    if(record.finished()) return;
    customList& output_bucket = next_node->buckets[tid];
    // the level finds at most the undiscovered vertices, and at most one per
    // edge leaving the frontier, which keeps long thin tails small; a thread
    // finds them in its own blocks unless it helps, and those hold about a
    // num_t-th of them. Sizing the bucket for that, with a quarter to spare,
    // saves copying it at every doubling. Capacity never written costs no
    // memory traffic
    long long frontier_degrees = 0;
    for(int t=0; t<num_t; t++) {
        const customList& bucket = curr->buckets[t];
        if(shared_load(bucket.level) != level) continue;
        compiler_barrier();
        frontier_degrees += shared_load(bucket.degree_sum);
    }
    long long share = max(0LL, min(undiscovered, frontier_degrees) / num_t);
    output_bucket.reserve(static_cast<int>(share + share / 4));

    fetch_own_blocks(curr, level, output_bucket, tid, code);
    fetch_other_blocks(curr, next_node, record, level, output_bucket, tid, rng, code);
}

// the thresholds dir-bfs.cpp uses, so both switch at the same frontier sizes
constexpr long long gapbs_alpha = 15;
constexpr long long gapbs_beta = 18;

[[gnu::always_inline]] inline level_direction choose_direction(
        const level_plan& before, long long frontier_size, long long scout) {
    // stay bottom-up while the frontier grows or is still large
    if(before.direction == bottom_up &&
            (frontier_size >= before.frontier_size || frontier_size > N / gapbs_beta)) {
        return bottom_up;
    }
    // go bottom-up once the frontier's edges are a large share of those unexplored
    if(scout > before.edges_to_check / gapbs_alpha) return bottom_up;
    return top_down;
}

// a bottom-up level after a bottom-up one tests the code its discoveries wrote,
// and any other bottom-up level takes the next unused code, which its first act
// writes for the frontier; either way its own discoveries take the code after
// it. Codes only grow within a search, so none is ever reused
[[gnu::always_inline]] inline void assign_code(
        const level_plan& before, level_plan& plan) {
    plan.code = 0;
    plan.last_code = before.last_code;
    if(plan.direction != bottom_up) return;
    int code = before.direction == bottom_up && before.code > 0
        ? before.code + 1 : before.last_code + 1;
    if(code <= max_code) {
        plan.code = code;
        plan.last_code = min(code + 1, max_code);
    }
}

// what a level's buckets hold: their entries, copies included, and the summed
// degrees that their owners counted (see add_degrees). Only a thread that finds
// the level undecided reads them, and it has followed the level before (see
// wf_bfs), so its view of the buckets is sound: owners reuse them for the level
// two on only once the level after this one is decided
struct frontier_counts {
    long long size = 0;
    long long degrees = 0;
};

frontier_counts read_frontier(const outer_list_node* curr, int level) {
    frontier_counts counts;
    for(int t=0; t<num_t; t++) {
        const customList& bucket = curr->buckets[t];
        if(shared_load(bucket.level) != level) continue;
        compiler_barrier();
        counts.size += shared_load(bucket.size);
        counts.degrees += shared_load(bucket.degree_sum);
    }
    return counts;
}

// the plan of a level from its frontier and the plan of the level before. An
// empty level is top-down and ends the search
level_plan plan_for(const level_plan& before, const frontier_counts& frontier) {
    // the edge count restarts at 1 after bottom-up levels, as GAPBS's does
    long long scout = before.direction == bottom_up ? 1 : frontier.degrees;
    level_direction direction = frontier.size == 0
        ? top_down : choose_direction(before, frontier.size, scout);
    // a top-down level explores the frontier's edges
    level_plan plan = {direction, before.edges_to_check - (direction == top_down ? scout : 0),
                       frontier.size, before.undiscovered - frontier.size, 0, 0};
    assign_code(before, plan);
    return plan;
}

// the record of the level whose slot this is: the one installed there, or, if
// the slot is empty, the first that a thread installs in it, which may be this
// thread's. Every thread must expand a level the same way, since its progress
// entries hold bucket positions top-down and block positions bottom-up, so only
// the first CAS counts. No other thread has seen a record whose CAS failed, so
// its thread keeps it for the next level it proposes for
level_record* decide(atomic<level_record*>& slot, const outer_list_node* curr, int level,
                     const level_plan& before, int tid, level_record*& spare) {
    level_record* seen = slot.load();
    if(seen != nullptr) return seen;
    level_record* mine = spare != nullptr
        ? spare : new(record_arenas[tid].allocate(sizeof(level_record))) level_record;
    spare = nullptr;
    mine->plan = plan_for(before, read_frontier(curr, level));
    mine->next.store(nullptr, memory_order_relaxed);
    // a failed swap leaves the record that another thread installed in seen
    if(slot.compare_exchange_strong(seen, mine)) return mine;
    spare = mine;
    return seen;
}

// this thread's part of a level: its own progress and next bucket first, then the
// level's work in the direction of its plan
void expand_level(
        outer_list_node* curr, outer_list_node* next_node, const level_record& record,
        int level, int tid, mt19937& rng, level_direction previous) {
    // helpers go by this thread's progress once its next bucket has the
    // next level (see last_done_on), so the old positions must go first
    shared_store(curr->progress[tid].last_done, -1);
    shared_store(curr->codes_progress[tid].last_done, -1);
    compiler_barrier();
    next_node->buckets[tid].begin_level(level + 1);

    const level_plan& plan = record.plan;
    if(plan.direction == bottom_up) {
        bottom_up_level(curr, next_node, record, level, tid, rng, plan.undiscovered,
                        previous, plan.code);
    }
    else {
        top_down_level(curr, next_node, record, level, tid, rng);
    }
}

void wf_bfs(outer_list_node* nodes, int tid, mt19937& rng) {
    // before the first level every edge is unexplored
    level_plan plan = {top_down, static_cast<long long>(offsets[N]), 1, N, 0, 0};
    level_record* spare = nullptr;
    atomic<level_record*>* slot = &first_slot;

    for(int level = 0; slot != nullptr; level++) {
        outer_list_node* curr = &nodes[level & 1];
        level_direction previous = plan.direction;
        level_record* record = decide(*slot, curr, level, plan, tid, spare);
        plan = record->plan;
        // a thread that has fallen behind takes the plan of a level that is over,
        // since the next level's codes depend on it, but starts none of it: it
        // appends nothing there, and others pass over its buckets, whose tags are
        // older
        if(!record->finished()) {
            expand_level(curr, &nodes[(level + 1) & 1], *record, level, tid, rng, previous);
        }
        // an empty level ends the search
        slot = record->ends_search() ? nullptr : &record->next;
    }
}

void init(int N, size_t M) {
    dist.resize(N, -1);
    dist8.resize(N, 0);
    expanded.resize(N, 0);
    block_count = (N + bottom_up_block - 1) / bottom_up_block;
}

// fills each vertex's neighbors in file order, so the lists match wf-bfs.cpp's
void load_graph(const string& path) {
    graph_helpers::edge_list edges = graph_helpers::read_graph(path);
    N = edges.n;
    M = edges.from.size();
    init(N, M);
    graph_helpers::build_csr(edges, offsets, adj);
    // first_chunk(u) + k stays below this for every chunk k of every u
    chunk_done.assign(static_cast<size_t>(N) + offsets[N] / hub_chunk + 1, 0);
}

// the two nodes, even levels in the first and odd ones in the second; they live
// for the whole run, and each search resets them
outer_list_node* nodes;

// a search starts from the source, the one entry of its first level, whose slot
// is empty. The threads are idle while it is set up, so its first array can come
// from thread 0's arena
void start_search() {
    nodes[0].begin_search();
    nodes[1].begin_search();
    first_slot.store(nullptr, memory_order_relaxed);
    customList& first = nodes[0].buckets[0];
    first.begin_level(0);
    first.push_back(source);
    first.degree_sum = degree(source);
}

// only once every thread has returned, since until then one may still be
// reading a bucket
void free_buckets() {
    for(int t=0; t<num_t; t++) {
        arenas[t].reset();
        record_arenas[t].reset();
    }
}

// thread tid's share of what a search starts from: a slice of dist, dist8 and
// expanded, with the source at 0, and its own row of helped_at, whose entries left
// from the last search would match this one's levels
void reset_share(int tid) {
    int begin = static_cast<int>(1LL * N * tid / num_t);
    int end = static_cast<int>(1LL * N * (tid + 1) / num_t);
    fill(dist.begin() + begin, dist.begin() + end, -1);
    fill(dist8.begin() + begin, dist8.begin() + end, 0);
    fill(expanded.begin() + begin, expanded.begin() + end, 0);
    if(begin <= source && source < end) dist[source] = 0;
    fill(helped_at.begin() + tid * blocks_per_owner,
         helped_at.begin() + (tid + 1) * blocks_per_owner, -1);
}

// the threads live for the whole run, as GAPBS's OpenMP threads do, so no
// search pays for starting them. The main thread sets up each search and
// publishes its number; every thread then resets its share, searches, and
// notes when it returned
struct search_control {
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

        wf_bfs(nodes, tid, rng);
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
    // sized once the thread count is known to be valid, since owners divide by it
    blocks_per_owner = owned_blocks(0);
    helped_at.assign(static_cast<size_t>(num_t) * blocks_per_owner, -1);

    arenas = vector<arena>(num_t);
    record_arenas = vector<arena>(num_t);
    // the searches of every thread count are numbered from 1
    fill(chunk_done.begin(), chunk_done.end(), 0);
    nodes = new outer_list_node[2];
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
        start_search();
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
        free_buckets();
    }

    searches.number = -1;
    for(thread& t: threads) t.join();
    delete[] nodes;

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

    try {
        write_output();
    }
    catch(const exception& error) {
        cerr << "Error: " << error.what() << "\n";
        return 1;
    }

    if(thread_counts.size() == 1) cout << average_duration << "\n";

    return 0;
}

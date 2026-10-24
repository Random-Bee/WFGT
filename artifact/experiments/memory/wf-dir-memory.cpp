#include <bits/stdc++.h>
#include <sys/resource.h>
#include "../../cpu_helpers/cpu_affinity.hpp"
#include "../../graph_helpers/graph_reader.hpp"
using namespace std;
using namespace chrono;

// The copy of WF-DO that prints the most bytes that the arenas handed out in a
// search, the bytes of their chunks, and the resident size of the process
// before a thread count's searches and at its peak during them, for memory.sh

int N;
size_t M;
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

    // the bytes handed out since the last reset
    size_t handed = 0;
    size_t chunk_total() const {
        size_t total = 0;
        for(const chunk& c: chunks) total += c.bytes;
        return total;
    }

    arena() = default;
    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;
    ~arena() {
        for(chunk& c: chunks) ::operator delete(c.begin);
    }

    void* allocate(size_t bytes) {
        bytes = (bytes + block_alignment - 1) & ~(block_alignment - 1);
        handed += bytes;
        uintptr_t start = align_up(next);
        if(next == nullptr || start + bytes > reinterpret_cast<uintptr_t>(end)) {
            start = take_chunk(bytes);
        }
        next = reinterpret_cast<char*>(start + bytes);
        return reinterpret_cast<void*>(start);
    }

    // only once no thread can still read a block it handed out
    void reset() {
        handed = 0;
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
    // summed degrees of the vertices pushed: the direction rule compares the
    // top-down sums with the edges still unexplored, and a bottom-up level
    // sizes its buckets by them
    long long degree_sum = 0;
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

enum level_direction { top_down, bottom_up };

// what the direction rule carries from one level to the next
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

// one thread's entries in a level. The first line holds what threads read as
// they walk the level: its bucket's array and size, and the flags, each set
// once. Only the owner changes the bucket, nearly always while the level before
// is expanded, but an owner stalled in that level can still push to it, and
// grow it, after other threads have started or finished this one. Such late
// entries are vertices the threads that finished the level before pushed too,
// so readers that miss them miss nothing. The owner rewrites its progress after
// every entry it finishes, so that and its proposal go on the second line,
// where those writes don't take the first from the readers.
// Each entry fills its own pair of lines, which the L2 prefetcher fetches
// together, so no two threads' entries share one
struct alignas(128) owner_entry {
    // only the owner pushes to it
    customList bucket;
    // set by the thread that finishes the bucket top-down, or the owner's
    // blocks bottom-up
    uint8_t done = 0;
    // set by the thread that writes the last of the bucket's frontier codes
    uint8_t codes_done = 0;
    // how far the owner has got: the last entry of its bucket it finished
    // top-down, the last of its blocks bottom-up
    alignas(64) int last_done = -1;
    // how far the owner has written its bucket's frontier codes
    int codes_last_done = -1;
    // written before it is read, so it starts unset
    level_plan proposal;

    explicit owner_entry(arena* memory) : bucket(memory) {}
};
static_assert(sizeof(owner_entry) == 128, "an entry is a pair of lines");

// a level's per-thread entries are an array of num_t, built by new_level
class outer_list_node {
    public:
    int depth;
    owner_entry* owners;
    // the first thread to claim plan_owner decides, with its proposal
    atomic<int> plan_owner;
    atomic<outer_list_node*> next;
    outer_list_node() {
        plan_owner = -1;
        next = nullptr;
    }
};

[[gnu::always_inline]] inline bool level_done(outer_list_node* curr) {
    outer_list_node* next = curr->next.load();
    return next != nullptr && next->plan_owner.load() >= 0;
}

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

// a random order of size indices, affine; buffer is unused
affinePermutation random_order(
        size_t size, mt19937& rng, vector<uint32_t>& buffer) {
    (void)buffer;
    return affine_shuffle(size, rng);
}

// the index after index in order, an order of size indices
[[gnu::always_inline]] inline size_t next_index(
        const affinePermutation& order, size_t index, size_t size) {
    return next_affine_index(index, order.step, size);
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

// helpers walk what is left of a list in blocks of helper_block entries, the
// blocks in a random order and the entries inside a block in order. Single
// entries keep helpers that reach a small bucket together from walking the
// same entries in step
constexpr int helper_block = 1;

// push before marking: if this thread stops in between, v stays unmarked and
// another thread can still discover and push it. The code goes before the mark
// too, so a thread that finds v marked also finds its code; past the last code
// it writes 0, which an undiscovered v already has
[[gnu::always_inline]] inline void discover_vertex(
        int v, customList& output_bucket, int next_distance, uint8_t next_code) {
    output_bucket.push_back(v);
    release_store(dist8[v], next_code);
    release_store(dist[v], next_distance);
}

// the owner's running sum, which planners read at the same time
[[gnu::always_inline]] inline void add_degrees(customList& bucket, long long degrees) {
    shared_store(bucket.degree_sum, shared_load(bucket.degree_sum) + degrees);
}

[[gnu::always_inline]] inline long long degree(int v) {
    return offsets[v+1] - offsets[v];
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

[[gnu::always_inline]] inline int stalls_class(int owner) {
    return 0;
}

// whether thread tid waits for owner instead of helping it
[[gnu::always_inline]] inline bool waits_for(int owner, int tid) {
    (void)owner;
    (void)tid;
    return false;
}


void process_neighbors(
        int u, customList& output_bucket, int next_distance) {
    const int* neighbors = adj.data() + offsets[u];
    size_t count = offsets[u+1] - offsets[u];
    for(size_t j=0; j<count; j++) {
        int v = neighbors[j];
        if(acquire_load(dist[v]) == -1) {
            discover_and_count(v, output_bucket, next_distance);
        }
    }
}

// helpers walk lists of up to three blocks in order, as owners do: a thread is
// on one only briefly, and shuffling it would cost two random draws
constexpr size_t ordered_list_max = 3 * helper_block;

// a helper walks a long list as it walks a bucket: in blocks of helper_block
// entries, the blocks in a random order of its own. An owner and helpers that
// expand the same hub then swap different entries of dist at any moment;
// walking the list in the same order, they would all swap the same ones, each
// swap waiting for the line
void process_neighbors_shuffled(
        int u, customList& output_bucket, int next_distance, mt19937& rng) {
    const int* neighbors = adj.data() + offsets[u];
    size_t count = offsets[u+1] - offsets[u];
    if(count <= ordered_list_max) {
        process_neighbors(u, output_bucket, next_distance);
        return;
    }
    size_t blocks = (count + helper_block - 1) / helper_block;
    thread_local vector<uint32_t> buffer;
    affinePermutation order = random_order(blocks, rng, buffer);
    size_t block = order.index;
    for(size_t b=0; b<blocks; b++) {
        size_t end = min((block + 1)*helper_block, count);
        for(size_t j=block*helper_block; j<end; j++) {
            int v = neighbors[j];
            if(acquire_load(dist[v]) == -1) {
                discover_and_count(v, output_bucket, next_distance);
            }
        }
        block = next_index(order, block, blocks);
    }
}

// a list longer than hub_chunk is expanded in chunks of that many neighbors,
// each certified once it is done, so that the threads on a hub share its list
// rather than each scanning all of it. The certificate is the number of the
// search, written after the chunk's discoveries: a thread skips a chunk
// certified in this search, and one that stops leaves only the chunk it is on
// to be scanned again; hub_chunk is both the size of a chunk and the degree
// above which a list is chunked
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

// the neighbors [begin, end) of a list, in order, or with rng in blocks of
// helper_block entries in a random order, as process_neighbors_shuffled walks a
// whole list: threads that scan one chunk at once then swap different entries
// of dist rather than each waiting for the same line
[[gnu::always_inline]] inline void process_neighbor_range(
        const int* neighbors, size_t begin, size_t end, customList& output_bucket,
        int next_distance, mt19937* rng) {
    const size_t blocks = (end - begin + helper_block - 1) / helper_block;
    if(rng == nullptr || blocks < 2) {
        for(size_t j=begin; j<end; j++) {
            int v = neighbors[j];
            if(acquire_load(dist[v]) == -1) {
                discover_and_count(v, output_bucket, next_distance);
            }
        }
        return;
    }
    thread_local vector<uint32_t> buffer;
    affinePermutation order = random_order(blocks, *rng, buffer);
    size_t block = order.index;
    for(size_t b=0; b<blocks; b++) {
        const size_t first = begin + block * helper_block;
        const size_t last = min(first + helper_block, end);
        for(size_t j=first; j<last; j++) {
            int v = neighbors[j];
            if(acquire_load(dist[v]) == -1) {
                discover_and_count(v, output_bucket, next_distance);
            }
        }
        block = next_index(order, block, blocks);
    }
}

// the chunks of u in order, an owner's in order from a chunk of its own and a
// helper's, given rng, in a random order, then u's mark: by then every chunk
// is certified, by this thread or another
void expand_chunked(
        int tid, int owner, int u, customList& output_bucket, int next_distance,
        const affinePermutation& order, mt19937* rng) {
    (void)tid;
    (void)owner;
    const int* neighbors = adj.data() + offsets[u];
    const size_t count = static_cast<size_t>(degree(u));
    const size_t chunks = chunk_count(u);
    int* certificates = chunk_done.data() + first_chunk(u);
    size_t k = order.index;
    for(size_t c=0; c<chunks; c++) {
        if(acquire_load(certificates[k]) != chunk_stamp) {
            const size_t begin = k * hub_chunk;
            const size_t end = min(begin + hub_chunk, count);
            process_neighbor_range(neighbors, begin, end, output_bucket, next_distance, rng);
            // others skip a certified chunk, so its neighbors must be pushed first
            release_store(certificates[k], chunk_stamp);
        }
        k = next_index(order, k, chunks);
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
        if(chunked(u)) {
            if(!acquire_load(expanded[u])) {
                // every thread that finds a hub undiscovered pushes it, so a hub can
                // be in many buckets, and owners that walked its chunks from the
                // same one would scan the same chunks at the same time
                const affinePermutation in_order{static_cast<size_t>(tid) * chunk_count(u) / num_t, 1};
                expand_chunked(tid, tid, u, output_bucket, shared_load(dist[u]) + 1,
                               in_order, nullptr);
            }
        }
        else
        if(!acquire_load(expanded[u])) {
            int du = shared_load(dist[u]);
            process_neighbors(u, output_bucket, du+1);
            // others skip a marked vertex, so its neighbors must be pushed first
            release_store(expanded[u], 1);
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
    int remaining = sz - start;

    size_t blocks = (remaining + helper_block - 1) / helper_block;
    thread_local vector<uint32_t> buffer;
    affinePermutation order = random_order(blocks, rng, buffer);
    size_t block = order.index;

    const int* bucket = acquire_load(curr->owners[work_on].bucket.list) + start;
    customList& output_bucket = next_node->owners[tid].bucket;

    for(size_t b=0; b<blocks; b++) {
        int base = static_cast<int>(block)*helper_block;
        int end = min(base + helper_block, remaining);
        for(int i=base; i<end; i++) {
            int u = shared_load(bucket[i]);
            if(acquire_load(expanded[u])) continue;

            int du = shared_load(dist[u]);
            if(chunked(u)) {
                thread_local vector<uint32_t> chunk_buffer;
                const affinePermutation chunk_order = random_order(chunk_count(u), rng, chunk_buffer);
                expand_chunked(tid, work_on, u, output_bucket, du+1, chunk_order, &rng);
                continue;
            }
            process_neighbors_shuffled(u, output_bucket, du+1, rng);
            // others skip a marked vertex, so its neighbors must be pushed first
            release_store(expanded[u], 1);
        }
        block = next_index(order, block, blocks);
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
        if(shared_load(dist[neighbors[j]]) == depth) {
            return true;
        }
    }
    return false;
}

// has_frontier_neighbor on dist8, for a level with a code
[[gnu::always_inline]] inline bool has_frontier_neighbor8(
        const int* neighbors, size_t count, uint8_t code) {
    for(size_t j=0; j<count; j++) {
        if(shared_load(dist8[neighbors[j]]) == code) {
            return true;
        }
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
        n += acquire_load(dist[u]) == -1;
    }
    if(code > 0) {
        // what this level finds is the next level's frontier, under the next
        // code, which the plan reserved for it
        uint8_t frontier_code = static_cast<uint8_t>(code);
        uint8_t next_code = code < max_code ? static_cast<uint8_t>(code + 1) : 0;
        for(int i=0; i<n; i++) {
            int u = candidates[i];
            // read once: GCC does not reuse loads across the release stores in
            // discover_vertex, and would read the offsets again for the degree
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

// the owners a thread visits in a level: itself, then each group of the others
// in a random order drawn for every walk. In a fixed order, threads that run
// out of work at about the same time walk the same buckets together and expand
// the same entries. owner(k) must be called for k = 0, 1, ... in turn,
// or from 1 on to leave the thread itself out
// a thread walks the owners of one node at a time, so one pair of buffers serves
thread_local vector<uint32_t> near_buffer, far_buffer;

struct ownerWalk {
    int tid;
    int near;
    affinePermutation near_order;
    affinePermutation far_order;

    ownerWalk(int thread, mt19937& rng) : tid(thread) {
        near = near_owners[tid];
        near_order = random_order(static_cast<size_t>(near), rng, near_buffer);
        far_order = random_order(static_cast<size_t>(num_t - 1 - near), rng, far_buffer);
    }

    int owner(int k) {
        int position = k;
        if(k > near) {
            position = 1 + near + static_cast<int>(far_order.index);
            far_order.index = next_index(far_order, far_order.index, static_cast<size_t>(num_t - 1 - near));
        }
        else if(k > 0) {
            position = 1 + static_cast<int>(near_order.index);
            near_order.index = next_index(near_order, near_order.index, static_cast<size_t>(near));
        }
        return help_order[static_cast<size_t>(tid) * num_t + position];
    }
};

// own blocks, in order, publishing each as the last one done and skipping those
// a helper got to first; a helper that finishes the rest marks the owner done,
// which stops it
void fetch_own_blocks(
        outer_list_node* curr, customList& output_bucket, int tid, int code) {
    int depth = curr->depth;
    int count = owned_blocks(tid);
    int& last_done = curr->owners[tid].last_done;
    for(int k=0; k<count && !acquire_load(curr->owners[tid].done); k++) {
        if(acquire_load(helped_at_level(tid, k)) != depth) {
            scan_block(owned_block(tid, k), depth, code, output_bucket);
        }
        // helpers skip blocks up to here, so their vertices must be marked first
        release_store(last_done, k);
    }
    if(!shared_load(curr->owners[tid].done)) release_store(curr->owners[tid].done, uint8_t(1));
}

// the blocks each other owner hasn't done yet, in a random order as top-down
// helpers take bucket entries, so helpers on the same owner spread out; blocks
// the owner or another helper has done since are skipped, and a helper that
// gets through the rest marks the owner done
void fetch_other_blocks(
        outer_list_node* curr, customList& output_bucket, int tid,
        mt19937& rng, int code) {
    int depth = curr->depth;
    ownerWalk walk(tid, rng);
    for(int k=1; k<num_t; k++) {
        if(level_done(curr)) return;
        int owner = walk.owner(k);
        if(acquire_load(curr->owners[owner].done)) continue;
        if(waits_for(owner, tid)) {
            while(!acquire_load(curr->owners[owner].done)) __builtin_ia32_pause();
            continue;
        }
        int start = acquire_load(curr->owners[owner].last_done) + 1;
        int remaining = owned_blocks(owner) - start;
        thread_local vector<uint32_t> buffer;
        affinePermutation order = random_order(static_cast<size_t>(max(remaining, 0)), rng, buffer);
        size_t index = order.index;
        for(int i=0; i<remaining; i++) {
            // read again for every block, since the owner and other helpers go on
            if(acquire_load(curr->owners[owner].done)) break;
            int block = start + static_cast<int>(index);
            if(block > acquire_load(curr->owners[owner].last_done) &&
                    acquire_load(helped_at_level(owner, block)) != depth) {
                scan_block(owned_block(owner, block), depth, code, output_bucket);
                // others skip the block once this is set, so its vertices must be marked first
                release_store(helped_at_level(owner, block), depth);
            }
            index = next_index(order, index, static_cast<size_t>(remaining));
        }
        if(!shared_load(curr->owners[owner].done)) release_store(curr->owners[owner].done, uint8_t(1));
    }
}

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

    for(int k=0; k<num_t; k++) {
        if(level_done(curr)) return;
        int work_on = walk.owner(k);
        if(acquire_load(curr->owners[work_on].done) ||
                acquire_load(curr->owners[work_on].bucket.size) == 0) {
            continue;
        }

        if(next_is_null && curr->next == nullptr) {
            add_node(curr, tid);
        }
        next_is_null = false;
        if(waits_for(work_on, tid)) {
            while(!acquire_load(curr->owners[work_on].done)) __builtin_ia32_pause();
            continue;
        }

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

// writes `code` for a frontier that top-down pushes left without one: this
// thread's own bucket in order, publishing how far it got, then whatever is left
// of each bucket no thread has finished. Every bucket is done when it returns,
// so the frontier test can start; a code written twice or late is still right,
// since every entry of the level's buckets is at the level
void write_frontier_codes(outer_list_node* curr, int tid, int code, mt19937& rng) {
    uint8_t frontier_code = static_cast<uint8_t>(code);
    ownerWalk walk(tid, rng);
    for(int k=0; k<num_t; k++) {
        if(level_done(curr)) return;
        int owner = walk.owner(k);
        if(acquire_load(curr->owners[owner].codes_done)) continue;
        if(waits_for(owner, tid)) {
            while(!acquire_load(curr->owners[owner].codes_done)) __builtin_ia32_pause();
            continue;
        }
        const customList& bucket = curr->owners[owner].bucket;
        // the vertices up to this size, and the array that holds them, are seen
        // after it (see push_back)
        int sz = acquire_load(bucket.size);
        const int* list = acquire_load(bucket.list);
        int& progress = curr->owners[owner].codes_last_done;
        if(owner == tid) {
            for(int i=0; i<sz; i++) {
                shared_store(dist8[shared_load(list[i])], frontier_code);
                // helpers start past here, so the code must be written first
                release_store(progress, i);
            }
        }
        else {
            int first = shared_load(progress) + 1;
            for(int i=first; i<sz; i++) {
                shared_store(dist8[shared_load(list[i])], frontier_code);
            }
        }
        // others skip the bucket once this is set, so its codes must be written first
        release_store(curr->owners[owner].codes_done, uint8_t(1));
    }
}

void bottom_up_level(
        outer_list_node* curr, int tid, mt19937& rng, long long undiscovered,
        level_direction previous, int code) {
    if(level_done(curr)) return;
    if(curr->next == nullptr) {
        add_node(curr, tid);
    }
    // after a top-down level the code is fresh, and nothing has written it yet
    if(previous == top_down && code > 0) {
        write_frontier_codes(curr, tid, code, rng);
    }
    if(level_done(curr)) return;
    outer_list_node* next_node = curr->next.load();
    customList& output_bucket = next_node->owners[tid].bucket;
    // the level finds at most the undiscovered vertices, and at most one per
    // edge leaving the frontier, which keeps long thin tails small; a thread
    // finds them in its own blocks unless it helps, and those hold about a
    // num_t-th of them. Sizing the bucket for that, with a quarter to spare,
    // saves copying it at every doubling. Capacity never written costs no
    // memory traffic
    long long frontier_degrees = 0;
    for(int t=0; t<num_t; t++) frontier_degrees += shared_load(curr->owners[t].bucket.degree_sum);
    long long share = min(undiscovered, frontier_degrees) / num_t;
    output_bucket.reserve(static_cast<int>(share + share / 4));

    fetch_own_blocks(curr, output_bucket, tid, code);
    fetch_other_blocks(curr, output_bucket, tid, rng, code);
}

// the thresholds dir-bfs.cpp uses, so both switch at the same frontier sizes
constexpr long long gapbs_alpha = 15;
constexpr long long gapbs_beta = 18;

[[gnu::always_inline]] inline level_plan propose_plan(
        const level_plan& before, long long frontier_size, long long scout) {
    long long undiscovered = before.undiscovered - frontier_size;
    // an empty frontier ends the search, which only a top-down level detects
    if(frontier_size == 0) {
        return {top_down, before.edges_to_check, 0, undiscovered, 0, 0};
    }
    // stay bottom-up while the frontier grows or is still large
    if(before.direction == bottom_up &&
            (frontier_size >= before.frontier_size || frontier_size > N / gapbs_beta)) {
        return {bottom_up, before.edges_to_check, frontier_size, undiscovered, 0, 0};
    }
    // go bottom-up once the frontier's edges are a large share of those unexplored
    if(scout > before.edges_to_check / gapbs_alpha) {
        return {bottom_up, before.edges_to_check, frontier_size, undiscovered, 0, 0};
    }
    return {top_down, before.edges_to_check - scout, frontier_size, undiscovered, 0, 0};
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

// every thread must expand a level the same way, since its progress entries
// hold bucket positions top-down and block positions bottom-up; each thread
// proposes a plan from what it sees, and the first to claim the level decides
level_plan plan_level(
        outer_list_node* curr, const level_plan& before, int tid) {
    int owner = curr->plan_owner.load();
    if(owner >= 0) return curr->owners[owner].proposal;

    long long frontier_size = 0;
    for(int t=0; t<num_t; t++) frontier_size += shared_load(curr->owners[t].bucket.size);

    // the edge count restarts at 1 after bottom-up levels, as GAPBS's does
    long long scout = 1;
    if(before.direction == top_down) {
        scout = 0;
        for(int t=0; t<num_t; t++) scout += shared_load(curr->owners[t].bucket.degree_sum);
    }

    level_plan plan = propose_plan(before, frontier_size, scout);
    assign_code(before, plan);
    curr->owners[tid].proposal = plan;
    if(!curr->plan_owner.compare_exchange_strong(owner, tid)) {
        return curr->owners[owner].proposal;
    }
    return curr->owners[tid].proposal;
}

void wf_bfs(outer_list_node* head, int tid, mt19937& rng) {
    // before the first level every edge is unexplored
    level_plan plan = {top_down, static_cast<long long>(offsets[N]), 1, N, 0, 0};
    outer_list_node* curr = head;

    while(curr != nullptr) {
        level_direction previous = plan.direction;
        plan = plan_level(curr, plan, tid);
        if(plan.direction == bottom_up) {
            bottom_up_level(curr, tid, rng, plan.undiscovered, previous, plan.code);
        }
        else {
            top_down_level(curr, tid, rng);
        }
        curr = curr->next;
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

// a search starts from vertex 0, the one entry of its first level. The threads
// are idle while it is set up, so it can come from thread 0's arenas
outer_list_node* make_first_level() {
    outer_list_node* head = new_level(0, node_arenas[0]);
    head->owners[0].bucket.push_back(0);
    head->owners[0].bucket.degree_sum = degree(0);
    return head;
}

// the most bytes that the arenas handed out in one search of a thread count;
// nothing is freed during a search, so a search holds the most at its end
size_t search_bytes = 0;

// a field of /proc/self/status, such as VmRSS or VmHWM, in kB
long long status_kb(const string& field) {
    ifstream status("/proc/self/status");
    string line;
    while(getline(status, line)) {
        if(line.compare(0, field.size() + 1, field + ":") == 0) {
            return stoll(line.substr(field.size() + 1));
        }
    }
    return -1;
}

// from here on, VmHWM is the peak of what follows
void reset_peak_rss() {
    ofstream("/proc/self/clear_refs") << "5";
}

// only once every thread has returned, since until then one may still be
// reading a bucket
void free_levels() {
    size_t total = 0;
    for(int t=0; t<num_t; t++) {
        total += arenas[t].handed + node_arenas[t].handed;
    }
    search_bytes = max(search_bytes, total);
    for(int t=0; t<num_t; t++) {
        arenas[t].reset();
        node_arenas[t].reset();
    }
}

// thread tid's share of what a search starts from: a slice of dist, dist8 and
// expanded, with the source at 0, its own row of helped_at, whose entries left from
// the last search would match this one's levels, and no spare level, since the
// arenas took the last one back
void reset_share(int tid) {
    spare_level = nullptr;
    int begin = static_cast<int>(1LL * N * tid / num_t);
    int end = static_cast<int>(1LL * N * (tid + 1) / num_t);
    fill(dist.begin() + begin, dist.begin() + end, -1);
    fill(dist8.begin() + begin, dist8.begin() + end, 0);
    fill(expanded.begin() + begin, expanded.begin() + end, 0);
    if(begin == 0 && end > 0) dist[0] = 0;
    fill(helped_at.begin() + tid * blocks_per_owner,
         helped_at.begin() + (tid + 1) * blocks_per_owner, -1);
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
    // what the process holds before this thread count's searches, the graph and
    // the per-vertex arrays among it; from here on, its peak
    long long rss_before = status_kb("VmRSS");
    reset_peak_rss();
    search_bytes = 0;
    build_help_order(cpu_ids);
    // sized once the thread count is known to be valid, since owners divide by it
    blocks_per_owner = owned_blocks(0);
    helped_at.assign(static_cast<size_t>(num_t) * blocks_per_owner, -1);

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
    // the levels come from node_arenas, which search_bytes counts too
    size_t chunk_bytes = 0;
    for(const arena& a: arenas) chunk_bytes += a.chunk_total();
    for(const arena& a: node_arenas) chunk_bytes += a.chunk_total();
    cerr << "memory search_bytes " << search_bytes << " chunk_bytes " << chunk_bytes
         << " node_bytes 0 rss_before_kb " << rss_before
         << " rss_peak_kb " << status_kb("VmHWM") << "\n";

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

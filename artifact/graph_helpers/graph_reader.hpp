#pragma once

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace graph_helpers {

// how many threads reading, building and writing use, whatever the search
// itself is given. More load faster, but the searches run afterwards slow
// down: on liveJournal they took about 4% longer after 8 threads had loaded
// the graph, and 7% after 16
constexpr int load_threads = 8;

// asks the kernel to back the untouched memory [p, p + bytes) with transparent
// huge pages once it is touched. A search streaming through a many-gigabyte
// array on 4 KB pages pays a page walk for every 4 KB; left alone, the kernel
// gives huge pages only if free ones happen to be at hand when a page is first
// touched, which depends on what else is allocated at the time. Only a hint:
// where huge pages are off, nothing changes
inline void advise_huge_pages(void* p, std::size_t bytes) {
    const std::uintptr_t page = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
    const std::uintptr_t first = reinterpret_cast<std::uintptr_t>(p);
    const std::uintptr_t begin = (first + page - 1) / page * page;
    const std::uintptr_t end = (first + bytes) / page * page;
    if(end > begin) {
        ::madvise(reinterpret_cast<void*>(begin), end - begin, MADV_HUGEPAGE);
    }
}

// std::allocator, except that resizing leaves new elements uninitialized
// rather than zeroing them; otherwise the main thread writes every page of a
// many-gigabyte array before the threads that fill it can start. Arrays of
// huge_page_bytes or more are put on huge pages
template<class T>
struct uninitialized_allocator: std::allocator<T> {
    static constexpr std::size_t huge_page_bytes = std::size_t(32) << 20;

    template<class U>
    struct rebind {
        using other = uninitialized_allocator<U>;
    };

    uninitialized_allocator() = default;
    template<class U>
    uninitialized_allocator(const uninitialized_allocator<U>&) noexcept {}

    T* allocate(std::size_t n) {
        T* p = std::allocator<T>::allocate(n);
        if(n * sizeof(T) >= huge_page_bytes) advise_huge_pages(p, n * sizeof(T));
        return p;
    }

    template<class U>
    void construct(U* p) {
        ::new(static_cast<void*>(p)) U;
    }
    template<class U, class... Args>
    void construct(U* p, Args&&... args) {
        ::new(static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }
};

template<class T>
using array = std::vector<T, uninitialized_allocator<T>>;

// vertices are numbered 0 to n-1; edge i joins from[i] and to[i], and edges
// keep the order they have in the file
struct edge_list {
    int n = 0;
    array<int> from;
    array<int> to;
};

namespace detail {

// runs work(t) for t = 0 to count-1, on the calling thread and count-1 others;
// once all have finished, the first exception by t reaches the caller
template<class Work>
void run_on_threads(int count, const Work& work) {
    std::vector<std::exception_ptr> errors(count);
    auto run = [&work, &errors](int t) {
        try {
            work(t);
        }
        catch(...) {
            errors[t] = std::current_exception();
        }
    };

    std::vector<std::thread> threads;
    try {
        for(int t=1; t<count; t++) threads.emplace_back(run, t);
    }
    catch(...) {
        for(std::thread& thread: threads) thread.join();
        throw;
    }
    run(0);
    for(std::thread& thread: threads) thread.join();

    for(const std::exception_ptr& error: errors) {
        if(error) std::rethrow_exception(error);
    }
}

// a problem at a place in the file; its line is counted only once there is a
// problem, so the readers don't count lines as they go
struct parse_error {
    const char* at;
    std::string message;
};

// a whole file mapped into memory, which skips the copy fread makes
class mapped_file {
    public:
    explicit mapped_file(const std::string& file_path): path(file_path) {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if(fd < 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to open " + path);
        }

        struct stat info;
        if(::fstat(fd, &info) != 0) {
            int error = errno;
            ::close(fd);
            throw std::system_error(
                error, std::generic_category(), "Failed to stat " + path);
        }

        size = static_cast<std::size_t>(info.st_size);
        if(size > 0) {
            void* mapped = ::mmap(
                nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            int error = errno;
            ::close(fd);
            if(mapped == MAP_FAILED) {
                throw std::system_error(
                    error, std::generic_category(), "Failed to map " + path);
            }
            data = static_cast<const char*>(mapped);
        }
        else {
            ::close(fd);
        }
    }

    ~mapped_file() {
        if(size > 0) ::munmap(const_cast<char*>(data), size);
    }

    mapped_file(const mapped_file&) = delete;
    mapped_file& operator=(const mapped_file&) = delete;

    const char* begin() const {
        return data;
    }

    const char* end() const {
        return data + size;
    }

    std::runtime_error error(const parse_error& problem) const {
        std::size_t line = 1 + std::count(data, problem.at, '\n');
        return std::runtime_error(
            path + ":" + std::to_string(line) + ": " + problem.message);
    }

    std::runtime_error error(const std::string& message) const {
        return std::runtime_error(path + ": " + message);
    }

    private:
    std::string path;
    const char* data = nullptr;
    std::size_t size = 0;
};

// reads the text from pos up to end
struct cursor {
    const char* pos;
    const char* end;

    // skips blank lines first, so callers never see one
    bool at_end() {
        while(pos < end && is_space(*pos)) pos++;
        return pos == end;
    }

    char peek() const {
        return pos < end ? *pos : '\0';
    }

    bool starts_with(const std::string& text) const {
        return static_cast<std::size_t>(end - pos) >= text.size() &&
            std::equal(text.begin(), text.end(), pos);
    }

    // the next number on the current line
    std::int64_t read_number() {
        while(pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\r')) {
            pos++;
        }
        if(pos == end || !is_digit(*pos)) {
            throw parse_error{pos, "Expected a number"};
        }

        const char* start = pos;
        std::int64_t value = 0;
        while(pos < end && is_digit(*pos)) {
            value = value * 10 + (*pos - '0');
            if(value > INT_MAX) {
                throw parse_error{start, "Number does not fit in an int"};
            }
            pos++;
        }
        return value;
    }

    // whatever is left of the line, such as a weight, is ignored
    void skip_line() {
        while(pos < end && *pos != '\n') pos++;
        if(pos < end) pos++;
    }

    static bool is_digit(char c) {
        return c >= '0' && c <= '9';
    }

    static bool is_space(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }
};

// the vertex an id stands for, when the file's ids run from first_id to
// first_id + n - 1
inline int to_vertex(
        std::int64_t id, int first_id, std::int64_t n, const char* at) {
    if(id < first_id || id - first_id >= n) {
        throw parse_error{at,
            "Vertex id " + std::to_string(id) + " is outside " +
            std::to_string(first_id) + " to " +
            std::to_string(first_id + n - 1)};
    }
    return static_cast<int>(id - first_id);
}

// the "u v" lines from begin to the end of the file, split among the load
// threads at line starts; each thread keeps the edges of its part, and the
// parts are then joined in file order. edges.n covers every id up to the
// largest; lines starting with # are skipped when comments is set
inline edge_list parse_edges(
        const mapped_file& file, const char* begin, int first_id,
        std::int64_t n, bool comments) {
    const char* end = file.end();
    const int threads = load_threads;

    std::vector<const char*> starts(threads + 1, end);
    starts[0] = begin;
    for(int t=1; t<threads; t++) {
        const char* p = std::max(
            starts[t-1], begin + (end - begin) * t / threads);
        while(p > begin && p < end && p[-1] != '\n') p++;
        starts[t] = p;
    }

    struct part {
        std::vector<int> from;
        std::vector<int> to;
        int largest = -1;
    };
    std::vector<part> parts(threads);
    run_on_threads(threads, [&](int t) {
        // the parts sit side by side, so threads growing them in place would
        // keep taking each other's cache lines
        part out;
        cursor in{starts[t], starts[t+1]};
        while(!in.at_end()) {
            if(comments && in.peek() == '#') {
                in.skip_line();
                continue;
            }
            std::int64_t first = in.read_number();
            std::int64_t second = in.read_number();
            int u = to_vertex(first, first_id, n, in.pos);
            int v = to_vertex(second, first_id, n, in.pos);
            in.skip_line();
            out.from.push_back(u);
            out.to.push_back(v);
            out.largest = std::max(out.largest, std::max(u, v));
        }
        parts[t] = std::move(out);
    });

    std::vector<std::size_t> first_edge(threads + 1, 0);
    int largest = -1;
    for(int t=0; t<threads; t++) {
        first_edge[t+1] = first_edge[t] + parts[t].from.size();
        largest = std::max(largest, parts[t].largest);
    }
    if(first_edge[threads] > INT_MAX) {
        throw file.error("More edges than fit in an int");
    }

    edge_list edges;
    edges.n = largest + 1;
    edges.from.resize(first_edge[threads]);
    edges.to.resize(first_edge[threads]);
    run_on_threads(threads, [&](int t) {
        std::copy(parts[t].from.begin(), parts[t].from.end(),
                  edges.from.data() + first_edge[t]);
        std::copy(parts[t].to.begin(), parts[t].to.end(),
                  edges.to.data() + first_edge[t]);
        parts[t] = part();
    });
    return edges;
}

// m lines of "u v" from begin on, once the file has given the counts n and m
inline edge_list read_counted_body(
        const mapped_file& file, const char* begin, std::int64_t n,
        std::int64_t m, int first_id) {
    if(n <= 0) throw parse_error{begin, "The vertex count must be positive"};

    edge_list edges = parse_edges(file, begin, first_id, n, false);
    if(static_cast<std::int64_t>(edges.from.size()) != m) {
        throw file.error(
            "Expected " + std::to_string(m) + " edges but found " +
            std::to_string(edges.from.size()));
    }
    edges.n = static_cast<int>(n);
    return edges;
}

inline void append_number(std::string& out, int value) {
    char digits[10];
    int length = 0;
    unsigned magnitude = value < 0
        ? 0u - static_cast<unsigned>(value)
        : static_cast<unsigned>(value);
    do {
        digits[length++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while(magnitude != 0);

    if(value < 0) out.push_back('-');
    while(length > 0) out.push_back(digits[--length]);
}

} // namespace detail

// a first line "N M", then M lines "u v" with ids from first_id, as in
// liveJournal1.edges, with ids from 1, and the graphs gapbs_generate.cpp,
// hubs_generate.cpp, grid_generate.cpp and attach_path.sh write, with ids from 0
inline edge_list read_counted_edges(const std::string& path, int first_id) {
    detail::mapped_file file(path);
    try {
        detail::cursor in{file.begin(), file.end()};
        if(in.at_end()) {
            throw detail::parse_error{in.pos, "Expected an \"N M\" line"};
        }
        std::int64_t n = in.read_number();
        std::int64_t m = in.read_number();
        in.skip_line();
        return detail::read_counted_body(file, in.pos, n, m, first_id);
    }
    catch(const detail::parse_error& problem) {
        throw file.error(problem);
    }
}

// Matrix Market coordinate files, as SuiteSparse and Network Repository give
// them: a %%MatrixMarket line, % comments, a "rows cols entries" line, then one
// "row col" entry per line with ids from 1. A symmetric matrix stores each
// edge once
inline edge_list read_matrix_market(const std::string& path) {
    const std::string banner = "%%MatrixMarket matrix coordinate";
    detail::mapped_file file(path);
    try {
        detail::cursor in{file.begin(), file.end()};
        if(!in.starts_with(banner)) {
            throw detail::parse_error{
                in.pos, "Expected a line starting \"" + banner + "\""};
        }
        in.skip_line();
        while(!in.at_end() && in.peek() == '%') in.skip_line();
        if(in.at_end()) {
            throw detail::parse_error{
                in.pos, "Expected a \"rows cols entries\" line"};
        }

        const char* size_line = in.pos;
        std::int64_t rows = in.read_number();
        std::int64_t columns = in.read_number();
        std::int64_t entries = in.read_number();
        in.skip_line();
        if(rows != columns) {
            throw detail::parse_error{size_line,
                "A graph needs a square matrix, not " + std::to_string(rows) +
                " by " + std::to_string(columns)};
        }
        return detail::read_counted_body(file, in.pos, rows, entries, 1);
    }
    catch(const detail::parse_error& problem) {
        throw file.error(problem);
    }
}

// "u v" lines with ids from first_id and no counts, as in SNAP's files; lines
// starting with # are comments. n covers every id up to the largest, so ids
// the file never uses become vertices without edges
inline edge_list read_bare_edges(const std::string& path, int first_id) {
    detail::mapped_file file(path);
    try {
        edge_list edges = detail::parse_edges(
            file, file.begin(), first_id, INT_MAX, true);
        if(edges.from.empty()) throw file.error("Found no edges");
        return edges;
    }
    catch(const detail::parse_error& problem) {
        throw file.error(problem);
    }
}

// the reader is chosen by the file's name alone, so the file can be in any
// directory
inline edge_list read_graph(const std::string& path) {
    const std::string file = path.substr(path.find_last_of('/') + 1);
    if(file == "liveJournal1.edges") {
        return read_counted_edges(path, 1);
    }
    if(file == "road-road-usa.mtx" || file == "uk-2002.mtx") {
        return read_matrix_market(path);
    }
    if(file == "twitter-2010.txt") {
        return read_bare_edges(path, 0);
    }
    if(file == "kron-25.edges" || file == "urand-25.edges" || file == "hubs.edges" ||
       file == "grid.edges" || file == "lj-tail.edges" || file == "chain.edges") {
        return read_counted_edges(path, 0);
    }
    throw std::invalid_argument(
        "Unknown graph file \"" + file + "\"; expected liveJournal1.edges, "
        "road-road-usa.mtx, uk-2002.mtx, twitter-2010.txt, kron-25.edges, "
        "urand-25.edges, hubs.edges, grid.edges, lj-tail.edges or chain.edges");
}

// the vertex every search starts from: BFS_SOURCE if it is set, and 0 otherwise,
// so that all the programs search from the same vertex. Anything but a vertex
// of the graph is an error, since a run from a misread vertex would be wrong
inline int search_source(std::size_t n) {
    const char* text = std::getenv("BFS_SOURCE");
    if(text == nullptr || *text == '\0') return 0;
    char* end = nullptr;
    errno = 0;
    long long value = std::strtoll(text, &end, 10);
    if(errno != 0 || *end != '\0' || value < 0 || static_cast<unsigned long long>(value) >= n) {
        throw std::invalid_argument(
            "BFS_SOURCE must be a vertex, from 0 to " + std::to_string(n - 1));
    }
    return static_cast<int>(value);
}

// the symmetric CSR the searches use: vertex u's neighbors are adj[offsets[u]]
// up to, not including, adj[offsets[u+1]]. Each edge is listed at both its
// ends, in file order, just as a sequential build lists them
inline void build_csr(
        const edge_list& edges, array<std::size_t>& offsets, array<int>& adj) {
    const int n = edges.n;
    const std::size_t m = edges.from.size();
    const int* from = edges.from.data();
    const int* to = edges.to.data();
    const int threads = load_threads;
    auto first_edge = [m, threads](int t) { return m * t / threads; };

    // each edge gives two list entries, (u, v) for u's list and (v, u) for
    // v's. Thread t sorts the entries of its share of the edges by the thread
    // that owns the list, keeping file order, and every owner then counts and
    // fills its lists alone. Lists are owned in blocks of 2^shift vertices,
    // which must be fine: twitter puts 6.5% of its entries, more than one
    // thread's share, in 256 consecutive vertices
    int shift = 0;
    while(((static_cast<std::int64_t>(n) - 1) >> shift) >= (1 << 20)) shift++;
    const int blocks =
        static_cast<int>(((static_cast<std::int64_t>(n) - 1) >> shift) + 1);
    auto block_start = [n, shift](int b) {
        return static_cast<int>(std::min<std::int64_t>(
            static_cast<std::int64_t>(b) << shift, n));
    };

    // two entries per edge, and fewer than 2^31 edges, keep a count within
    // 32 bits
    std::vector<std::vector<std::uint32_t>> in_block(threads);
    detail::run_on_threads(threads, [&](int t) {
        std::vector<std::uint32_t> count(blocks, 0);
        for(std::size_t i=first_edge(t); i<first_edge(t+1); i++) {
            count[from[i] >> shift]++;
            count[to[i] >> shift]++;
        }
        in_block[t] = std::move(count);
    });

    // owners take runs of blocks holding about as many entries each
    std::vector<int> first_block(threads + 1, blocks);
    first_block[0] = 0;
    std::size_t seen = 0;
    for(int b=0, next_owner=1; b<blocks; b++) {
        for(int t=0; t<threads; t++) seen += in_block[t][b];
        while(next_owner < threads &&
                seen >= 2 * m * next_owner / threads) {
            first_block[next_owner++] = b + 1;
        }
    }
    // a byte per block keeps the table small enough to stay in cache while
    // the entries are sorted
    static_assert(load_threads <= 256, "owners are numbered in a byte");
    std::vector<std::uint8_t> owner_of(blocks);
    for(int o=0; o<threads; o++) {
        for(int b=first_block[o]; b<first_block[o+1]; b++) {
            owner_of[b] = static_cast<std::uint8_t>(o);
        }
    }

    // owner o's entries sit in region[o] up to region[o+1], thread 0's first
    std::vector<std::size_t> region(threads + 1);
    std::vector<std::vector<std::size_t>> place(
        threads, std::vector<std::size_t>(threads));
    std::size_t placed = 0;
    for(int o=0; o<threads; o++) {
        region[o] = placed;
        for(int t=0; t<threads; t++) {
            place[t][o] = placed;
            for(int b=first_block[o]; b<first_block[o+1]; b++) {
                placed += in_block[t][b];
            }
        }
    }
    region[threads] = placed;

    struct entry {
        int list;
        int neighbor;
    };
    array<entry> entries(2 * m);
    detail::run_on_threads(threads, [&](int t) {
        std::vector<std::size_t> next = place[t];
        for(std::size_t i=first_edge(t); i<first_edge(t+1); i++) {
            const int u = from[i], v = to[i];
            entries[next[owner_of[u >> shift]]++] = {u, v};
            entries[next[owner_of[v >> shift]]++] = {v, u};
        }
    });

    offsets.resize(static_cast<std::size_t>(n) + 1);
    offsets[0] = 0;
    std::size_t* degree = offsets.data() + 1;
    detail::run_on_threads(threads, [&](int o) {
        std::fill(degree + block_start(first_block[o]),
                  degree + block_start(first_block[o+1]), 0);
        for(std::size_t k=region[o]; k<region[o+1]; k++) {
            degree[entries[k].list]++;
        }
    });
    std::partial_sum(offsets.begin(), offsets.end(), offsets.begin());

    adj.resize(offsets[n]);
    int* neighbors = adj.data();
    detail::run_on_threads(threads, [&](int o) {
        const int lo = block_start(first_block[o]);
        const int hi = block_start(first_block[o+1]);
        std::vector<std::size_t> next(
            offsets.begin() + lo, offsets.begin() + hi);
        for(std::size_t k=region[o]; k<region[o+1]; k++) {
            neighbors[next[entries[k].list - lo]++] = entries[k].neighbor;
        }
    });
}

// one distance per line, then a blank line and the largest distance, as every
// program writes them; the load threads turn the numbers into text
inline void write_distances(
        const std::string& path, const std::vector<int>& dist) {
    const int threads = load_threads;
    const std::size_t count = dist.size();
    std::vector<std::string> text(threads);
    std::vector<int> largest(threads, INT_MIN);
    detail::run_on_threads(threads, [&](int t) {
        const std::size_t lo = count * t / threads;
        const std::size_t hi = count * (t + 1) / threads;
        // built apart from the shared vectors, whose entries share cache lines
        std::string out;
        out.reserve((hi - lo) * 4);
        int top = INT_MIN;
        for(std::size_t i=lo; i<hi; i++) {
            detail::append_number(out, dist[i]);
            out.push_back('\n');
            top = std::max(top, dist[i]);
        }
        text[t] = std::move(out);
        largest[t] = top;
    });

    std::FILE* f_out = std::fopen(path.c_str(), "w");
    if(f_out == nullptr) {
        throw std::system_error(
            errno, std::generic_category(), "Failed to open " + path);
    }
    for(const std::string& part: text) {
        std::fwrite(part.data(), 1, part.size(), f_out);
    }
    std::fprintf(
        f_out, "\n%d\n", *std::max_element(largest.begin(), largest.end()));
    bool failed = std::ferror(f_out) != 0;
    if(std::fclose(f_out) != 0 || failed) {
        throw std::runtime_error("Failed to write " + path);
    }
}

} // namespace graph_helpers

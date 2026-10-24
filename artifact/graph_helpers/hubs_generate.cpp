// Writes a graph whose BFS levels alternate between a few vertices of very high
// degree and many vertices of degree 2: a root, vertex 0, joined to the hubs of
// layer 0; then L layers of h hubs each; and between layers i and i+1, s spokes,
// each joined to one hub of layer i and one of layer i+1, both drawn uniformly at
// random. A search from vertex 0 reaches the hubs of layer i at level 2i+1 and
// the spokes after them at level 2i+2, so every other level holds h hubs with
// about 2s/h neighbors each. The labels other than 0 are a random permutation,
// so that the neighbors of a hub are spread over memory. The format is the one
// graph_reader.hpp reads for the GAPBS graphs: an "N M" line, then each edge
// once as "u v" with u < v, vertices numbered from 0, in increasing order of u
// and then v.
//
//   g++ -std=c++23 -O3 -Wall -Wextra hubs_generate.cpp -o hubs_generate
//   ./hubs_generate <layers> <hubs per layer> <spokes per gap> <seed> <output file>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <system_error>
#include <vector>

using namespace std;

namespace {

// the ChaCha20 keystream (the block function of RFC 8439) with the seed as its
// key, so that a seed always gives the same graph
class chacha20 {
public:
    explicit chacha20(uint64_t seed) {
        state = {0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u};
        state[4] = static_cast<uint32_t>(seed);
        state[5] = static_cast<uint32_t>(seed >> 32);
    }

    uint32_t next() {
        if(used == block.size()) refill();
        return block[used++];
    }

    // uniform in [0, n) for n > 0, by Lemire's multiply and reject
    uint32_t below(uint32_t n) {
        uint64_t product = static_cast<uint64_t>(next()) * n;
        uint32_t low = static_cast<uint32_t>(product);
        if(low < n) {
            const uint32_t threshold = (0u - n) % n;
            while(low < threshold) {
                product = static_cast<uint64_t>(next()) * n;
                low = static_cast<uint32_t>(product);
            }
        }
        return static_cast<uint32_t>(product >> 32);
    }

private:
    static uint32_t rotate(uint32_t x, int bits) { return (x << bits) | (x >> (32 - bits)); }

    static void quarter(uint32_t& a, uint32_t& b, uint32_t& c, uint32_t& d) {
        a += b; d ^= a; d = rotate(d, 16);
        c += d; b ^= c; b = rotate(b, 12);
        a += b; d ^= a; d = rotate(d, 8);
        c += d; b ^= c; b = rotate(b, 7);
    }

    void refill() {
        array<uint32_t, 16> x = state;
        for(int round=0; round<10; round++) {
            quarter(x[0], x[4], x[8], x[12]);
            quarter(x[1], x[5], x[9], x[13]);
            quarter(x[2], x[6], x[10], x[14]);
            quarter(x[3], x[7], x[11], x[15]);
            quarter(x[0], x[5], x[10], x[15]);
            quarter(x[1], x[6], x[11], x[12]);
            quarter(x[2], x[7], x[8], x[13]);
            quarter(x[3], x[4], x[9], x[14]);
        }
        for(size_t i=0; i<block.size(); i++) block[i] = x[i] + state[i];
        if(++state[12] == 0) ++state[13];
        used = 0;
    }

    array<uint32_t, 16> state{};
    array<uint32_t, 16> block{};
    size_t used = 16;
};

// a whole number from min to max, or false
bool parse(const char* text, long long min, long long max, long long& value) {
    const char* end = text + strlen(text);
    auto [stop, problem] = from_chars(text, end, value);
    return problem == errc() && stop == end && value >= min && value <= max;
}

void append_number(string& out, uint32_t value) {
    char digits[10];
    const to_chars_result written = to_chars(digits, digits + sizeof digits, value);
    out.append(digits, written.ptr);
}

}  // namespace

int main(int argc, char* argv[]) {
    long long layers = 0, hubs = 0, spokes = 0, seed = 0;
    if(argc != 6 || !parse(argv[1], 2, INT_MAX, layers) || !parse(argv[2], 1, INT_MAX, hubs) ||
            !parse(argv[3], 1, INT_MAX, spokes) || !parse(argv[4], 0, LLONG_MAX, seed)) {
        cerr << "Usage: " << argv[0]
             << " <layers, at least 2> <hubs per layer> <spokes per gap> <seed> <output file>\n";
        return 1;
    }
    // labels are ints in the programs that read the graph; with every count an
    // int, neither total overflows
    const long long n = 1 + layers * hubs + (layers - 1) * spokes;
    const long long m = hubs + 2 * spokes * (layers - 1);
    if(n > INT_MAX) {
        cerr << "Error: " << n << " vertices do not fit in an int\n";
        return 1;
    }
    if(m > (1LL << 32)) {
        cerr << "Error: " << m << " edges are more than this program writes, 2^32\n";
        return 1;
    }

    chacha20 random(static_cast<uint64_t>(seed));
    vector<uint32_t> label(n);
    iota(label.begin(), label.end(), 0u);
    for(long long i=n-1; i>1; i--) {
        swap(label[i], label[1 + random.below(static_cast<uint32_t>(i))]);
    }

    auto hub = [&](long long layer, long long j) { return 1 + layer * hubs + j; };
    auto spoke = [&](long long gap, long long k) { return 1 + layers * hubs + gap * spokes + k; };
    vector<uint64_t> edges;
    edges.reserve(m);
    vector<long long> hub_degree(layers * hubs, 0);
    auto add = [&](long long a, long long b) {
        uint32_t u = label[a], v = label[b];
        if(u > v) swap(u, v);
        edges.push_back((static_cast<uint64_t>(u) << 32) | v);
    };
    for(long long j=0; j<hubs; j++) {
        add(0, hub(0, j));
        hub_degree[j]++;
    }
    for(long long gap=0; gap<layers-1; gap++) {
        for(long long k=0; k<spokes; k++) {
            const long long left = hub(gap, random.below(static_cast<uint32_t>(hubs)));
            const long long right = hub(gap + 1, random.below(static_cast<uint32_t>(hubs)));
            add(spoke(gap, k), left);
            add(spoke(gap, k), right);
            hub_degree[left - 1]++;
            hub_degree[right - 1]++;
        }
    }
    sort(edges.begin(), edges.end());

    // written under another name first, so a run that stops partway leaves no
    // file under the final one
    const string path = argv[5];
    const string partial = path + ".partial";
    FILE* f = fopen(partial.c_str(), "w");
    if(f == nullptr) {
        cerr << "Error: cannot open " << partial << ": " << strerror(errno) << "\n";
        return 1;
    }
    bool failed = fprintf(f, "%lld %lld\n", n, m) < 0;
    string out;
    out.reserve(size_t(1) << 26);
    for(size_t e=0; e<edges.size() && !failed; e++) {
        append_number(out, static_cast<uint32_t>(edges[e] >> 32));
        out.push_back(' ');
        append_number(out, static_cast<uint32_t>(edges[e]));
        out.push_back('\n');
        if(out.size() >= (size_t(1) << 26) - 32 || e + 1 == edges.size()) {
            failed = fwrite(out.data(), 1, out.size(), f) != out.size();
            out.clear();
        }
    }
    failed = (fclose(f) != 0) || failed;
    if(failed || rename(partial.c_str(), path.c_str()) != 0) {
        cerr << "Error: failed to write " << path << ": " << strerror(errno) << "\n";
        remove(partial.c_str());
        return 1;
    }

    // the hubs of the first and last layers have spokes on one side only
    auto begin = hub_degree.begin(), end = hub_degree.end();
    if(layers > 2) {
        begin += hubs;
        end -= hubs;
    }
    const auto [fewest, most] = minmax_element(begin, end);
    cout << "Wrote " << path << ": " << n << " vertices, " << m << " edges; a search from vertex 0 "
         << "takes " << 2 * layers << " levels, and a hub" << (layers > 2 ? " of an inner layer" : "")
         << " has " << *fewest << " to " << *most << " neighbors\n";
    return 0;
}

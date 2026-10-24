// Writes a rows by cols torus, the grid that PASGAL's generate_grid_graph.cpp
// builds and searches symmetrized: vertex x * cols + y is joined to its four
// neighbors (x +- 1, y) and (x, y +- 1), wrapping around at the edges. A search
// from vertex 0 takes rows / 2 + cols / 2 + 1 levels, each of at most
// 2 min(rows, cols) vertices. The format is the one graph_reader.hpp reads for
// the GAPBS graphs: an "N M" line, then each edge once as "u v" with u < v,
// vertices numbered from 0, in increasing order of u and then v.
//
//   g++ -std=c++23 -O3 -Wall -Wextra grid_generate.cpp -o grid_generate
//   ./grid_generate <rows> <cols> <output file>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

using namespace std;

namespace {

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
    long long rows = 0, cols = 0;
    // with fewer than 3 in a dimension, wrapping around would repeat edges
    if(argc != 4 || !parse(argv[1], 3, INT_MAX, rows) || !parse(argv[2], 3, INT_MAX, cols)) {
        cerr << "Usage: " << argv[0] << " <rows, at least 3> <cols, at least 3> <output file>\n";
        return 1;
    }
    // labels are ints in the programs that read the graph
    if(rows * cols > INT_MAX) {
        cerr << "Error: " << rows * cols << " vertices do not fit in an int\n";
        return 1;
    }
    const long long n = rows * cols;
    const long long m = 2 * n;

    // each vertex's edges to the right and down, which together are every edge once
    vector<uint64_t> edges;
    edges.reserve(m);
    auto add = [&](long long a, long long b) {
        uint32_t u = static_cast<uint32_t>(a), v = static_cast<uint32_t>(b);
        if(u > v) swap(u, v);
        edges.push_back((static_cast<uint64_t>(u) << 32) | v);
    };
    for(long long x=0; x<rows; x++) {
        for(long long y=0; y<cols; y++) {
            const long long u = x * cols + y;
            add(u, x * cols + (y + 1) % cols);
            add(u, ((x + 1) % rows) * cols + y);
        }
    }
    sort(edges.begin(), edges.end());

    // written under another name first, so a run that stops partway leaves no
    // file under the final one
    const string path = argv[3];
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
    cout << "Wrote " << path << ": " << n << " vertices, " << m << " edges; a search from vertex 0 "
         << "takes " << rows / 2 + cols / 2 + 1 << " levels\n";
    return 0;
}

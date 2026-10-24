// Writes a GAPBS synthetic graph in a format graph_reader.hpp reads: the graph
// GAPBS's converter builds from the same flags (-g scale for Kronecker, -u scale
// for uniform random, -k degree, 16 if not given; generated graphs are
// undirected, without self-loops or repeated edges), as an "N M" line and then
// each undirected edge once as "u v" with u < v, vertices numbered from 0, in
// increasing order of u and then v. Every program here searches from vertex 0,
// so if vertex 0 is outside the largest component, it trades labels with the
// lowest-numbered vertex inside it.
//
// Built as GAPBS's Makefile builds its programs, from the GAPBS submodule:
//   g++ -std=c++11 -O3 -Wall -fopenmp -I ../submodules/gapbs/src gapbs_generate.cpp -o gapbs_generate
//   ./gapbs_generate -g 25 -e kron-25.edges

#include <omp.h>

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "benchmark.h"
#include "builder.h"
#include "command_line.h"
#include "graph.h"
#include "timer.h"

using namespace std;

namespace {

// marks the vertices reachable from source and returns how many there are
int64_t mark_component(const Graph& g, NodeID source, vector<uint8_t>& reached) {
    reached.assign(g.num_nodes(), 0);
    vector<NodeID> frontier{source}, next;
    reached[source] = 1;
    int64_t count = 1;
    while(!frontier.empty()) {
        next.clear();
        for(NodeID u: frontier) {
            for(NodeID v: g.out_neigh(u)) {
                if(!reached[v]) {
                    reached[v] = 1;
                    next.push_back(v);
                    count++;
                }
            }
        }
        frontier.swap(next);
    }
    return count;
}

inline void append_number(string& out, uint32_t value) {
    char digits[10];
    int length = 0;
    do {
        digits[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while(value != 0);
    while(length > 0) out.push_back(digits[--length]);
}

}  // namespace

int main(int argc, char* argv[]) {
    CLConvert cli(argc, argv, "gapbs_generate");
    if(!cli.ParseArgs()) return 1;
    if(cli.scale() == -1 || !cli.out_el() || cli.out_weighted()) {
        cerr << "Usage: " << argv[0]
             << " (-g <scale> | -u <scale>) [-k <degree>] -e <output file>\n";
        return 1;
    }

    Builder builder(cli);
    Graph g = builder.MakeGraph();
    g.PrintStats();
    const NodeID n = static_cast<NodeID>(g.num_nodes());

    // the largest component holds the vertex of highest degree
    NodeID hub = 0;
    for(NodeID u=1; u<n; u++) {
        if(g.out_degree(u) > g.out_degree(hub)) hub = u;
    }
    vector<uint8_t> in_largest;
    const int64_t largest = mark_component(g, hub, in_largest);
    int64_t isolated = 0;
    #pragma omp parallel for reduction(+ : isolated)
    for(NodeID u=0; u<n; u++) isolated += g.out_degree(u) == 0;

    // the vertex that takes label 0, and gives its own to vertex 0
    NodeID swapped = 0;
    while(!in_largest[swapped]) swapped++;
    cout << "Largest component: " << largest << " vertices; isolated vertices: "
         << isolated << "\n";
    if(swapped == 0) {
        cout << "Vertex 0 is in the largest component, degree " << g.out_degree(0) << "\n";
    }
    else {
        cout << "Vertex 0 (degree " << g.out_degree(0) << ") is outside it, so it trades "
             << "labels with vertex " << swapped << " (degree " << g.out_degree(swapped)
             << ")\n";
    }
    // trading two labels is its own inverse
    auto label = [swapped](NodeID u) {
        return u == 0 ? swapped : (u == swapped ? 0 : u);
    };

    int64_t m = 0;
    #pragma omp parallel for reduction(+ : m) schedule(dynamic, 1024)
    for(NodeID u=0; u<n; u++) {
        for(NodeID v: g.out_neigh(u)) m += u < v;
    }

    // the lines are formatted in parallel, a block of vertices at a time, and
    // written in order; each list is sorted by the old labels, so only a list
    // holding one of the two traded labels can need sorting again
    Timer t;
    t.Start();
    const NodeID block = NodeID(1) << 15;
    const NodeID blocks = (n + block - 1) / block;
    vector<string> text(blocks);
    #pragma omp parallel
    {
        vector<NodeID> higher;
        #pragma omp for schedule(dynamic, 1)
        for(NodeID b=0; b<blocks; b++) {
            string& out = text[b];
            for(NodeID u=b*block; u<min(n, (b+1)*block); u++) {
                NodeID old = label(u);
                const NodeID* begin = g.out_neigh(old).begin();
                const NodeID* end = g.out_neigh(old).end();
                higher.clear();
                for(const NodeID* w=begin; w<end; w++) {
                    NodeID v = label(*w);
                    if(v > u) higher.push_back(v);
                }
                if(swapped != 0 && (binary_search(begin, end, 0) ||
                        binary_search(begin, end, swapped))) {
                    sort(higher.begin(), higher.end());
                }
                for(NodeID v: higher) {
                    append_number(out, static_cast<uint32_t>(u));
                    out.push_back(' ');
                    append_number(out, static_cast<uint32_t>(v));
                    out.push_back('\n');
                }
            }
        }
    }

    // written under another name first, so a run that stops partway leaves no
    // file under the final one
    const string path = cli.out_filename();
    const string partial = path + ".partial";
    FILE* f = fopen(partial.c_str(), "w");
    if(f == nullptr) {
        cerr << "Error: cannot open " << partial << ": " << strerror(errno) << "\n";
        return 1;
    }
    bool failed = fprintf(f, "%d %" PRId64 "\n", n, m) < 0;
    for(string& part: text) {
        if(!failed && fwrite(part.data(), 1, part.size(), f) != part.size()) failed = true;
        string().swap(part);
    }
    failed = (fclose(f) != 0) || failed;
    if(failed || rename(partial.c_str(), path.c_str()) != 0) {
        cerr << "Error: failed to write " << path << ": " << strerror(errno) << "\n";
        return 1;
    }
    t.Stop();
    PrintTime("Write Time", t.Seconds());
    cout << "Wrote " << path << ": " << n << " vertices, " << m << " edges\n";
    return 0;
}

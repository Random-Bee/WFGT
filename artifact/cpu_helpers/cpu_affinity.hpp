#pragma once

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace cpu_helpers {
namespace detail {

struct CpuTimes {
    std::uint64_t idle;
    std::uint64_t total;
};

struct CpuCandidate {
    int cpu;
    int numa_node;
    std::string core_key;
    double idle_ratio;
};

inline std::map<int, CpuTimes> read_cpu_times() {
    std::ifstream stat_file("/proc/stat");
    if(!stat_file) {
        throw std::runtime_error("Failed to open /proc/stat");
    }

    std::map<int, CpuTimes> cpu_times;
    std::string line;
    while(std::getline(stat_file, line)) {
        std::istringstream input(line);
        std::string label;
        input >> label;

        if(label == "cpu") continue;
        if(label.size() <= 3 || label.compare(0, 3, "cpu") != 0) {
            if(!cpu_times.empty()) break;
            continue;
        }

        bool numeric_id = std::all_of(
            label.begin() + 3, label.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
        if(!numeric_id) continue;

        std::vector<std::uint64_t> ticks;
        std::uint64_t value;
        while(input >> value) ticks.push_back(value);
        if(ticks.size() < 4) continue;

        std::uint64_t total = 0;
        std::size_t accounted_fields =
            std::min<std::size_t>(ticks.size(), 8);
        for(std::size_t i=0; i<accounted_fields; i++) {
            total += ticks[i];
        }

        int cpu = std::stoi(label.substr(3));
        cpu_times[cpu] = {ticks[3], total};
    }

    if(cpu_times.empty()) {
        throw std::runtime_error(
            "No per-CPU statistics found in /proc/stat");
    }

    return cpu_times;
}

inline std::vector<int> parse_cpu_list(const std::string& cpu_list) {
    std::vector<int> cpus;
    std::istringstream input(cpu_list);
    std::string range;

    while(std::getline(input, range, ',')) {
        if(range.empty()) continue;

        std::size_t dash = range.find('-');
        int first = std::stoi(range.substr(0, dash));
        int last = dash == std::string::npos
            ? first
            : std::stoi(range.substr(dash + 1));

        if(first < 0 || last < first) continue;
        for(int cpu=first; cpu<=last && cpu<CPU_SETSIZE; cpu++) {
            cpus.push_back(cpu);
        }
    }

    return cpus;
}

struct DirectoryCloser {
    void operator()(DIR* directory) const {
        if(directory != nullptr) closedir(directory);
    }
};

inline std::map<int, int> read_numa_nodes() {
    std::map<int, int> cpu_to_node;
    std::unique_ptr<DIR, DirectoryCloser> node_directory(
        opendir("/sys/devices/system/node"));
    if(!node_directory) return cpu_to_node;

    dirent* entry;
    while((entry = readdir(node_directory.get())) != nullptr) {
        std::string name(entry->d_name);
        if(name.size() <= 4 || name.compare(0, 4, "node") != 0) {
            continue;
        }

        bool numeric_id = std::all_of(
            name.begin() + 4, name.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
        if(!numeric_id) continue;

        int node = std::stoi(name.substr(4));
        std::ifstream cpu_list_file(
            "/sys/devices/system/node/" + name + "/cpulist");
        std::string cpu_list;
        if(!std::getline(cpu_list_file, cpu_list)) continue;

        for(int cpu: parse_cpu_list(cpu_list)) {
            cpu_to_node[cpu] = node;
        }
    }

    return cpu_to_node;
}

inline std::string read_core_key(int cpu) {
    std::ifstream siblings_file(
        "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
        "/topology/thread_siblings_list");
    std::string siblings;
    if(std::getline(siblings_file, siblings) && !siblings.empty()) {
        return siblings;
    }
    return "cpu-" + std::to_string(cpu);
}

inline std::vector<CpuCandidate> choose_candidates(
        std::vector<CpuCandidate> candidates, std::size_t count) {
    std::sort(
        candidates.begin(), candidates.end(),
        [](const CpuCandidate& lhs, const CpuCandidate& rhs) {
            if(lhs.idle_ratio != rhs.idle_ratio) {
                return lhs.idle_ratio > rhs.idle_ratio;
            }
            return lhs.cpu < rhs.cpu;
        });

    std::vector<CpuCandidate> selected;
    std::set<std::string> selected_cores;
    std::set<int> selected_cpus;

    for(const CpuCandidate& candidate: candidates) {
        if(selected_cores.insert(candidate.core_key).second) {
            selected.push_back(candidate);
            selected_cpus.insert(candidate.cpu);
            if(selected.size() == count) return selected;
        }
    }

    for(const CpuCandidate& candidate: candidates) {
        if(selected_cpus.insert(candidate.cpu).second) {
            selected.push_back(candidate);
            if(selected.size() == count) return selected;
        }
    }

    return selected;
}

// count CPUs as an equal share from each node, the first nodes taking one
// more when the count does not divide evenly, dealt out so that consecutive
// threads go to different nodes; empty if a node lacks CPUs for its share
inline std::vector<int> spread_over_nodes(
        const std::map<int, std::vector<CpuCandidate>>& by_node,
        std::size_t count) {
    std::vector<std::vector<CpuCandidate>> shares;
    for(const auto& node: by_node) {
        std::size_t share = count / by_node.size() +
            (shares.size() < count % by_node.size() ? 1 : 0);
        shares.push_back(share == 0 ?
            std::vector<CpuCandidate>() : choose_candidates(node.second, share));
        if(shares.back().size() != share) return {};
    }

    std::vector<int> cpus;
    for(std::size_t round = 0; cpus.size() < count; round++) {
        for(const std::vector<CpuCandidate>& share: shares) {
            if(round < share.size()) cpus.push_back(share[round].cpu);
        }
    }
    return cpus;
}

} // namespace detail

// BFS_SPREAD_NODES set to anything but 0 spreads the threads evenly over the
// NUMA nodes, where by default they all go on one node whenever one has room
inline bool spread_threads() {
    const char* value = std::getenv("BFS_SPREAD_NODES");
    return value != nullptr && *value != '\0' && std::string(value) != "0";
}

inline std::vector<int> select_idle_cpus(
        int thread_count,
        std::chrono::milliseconds sample_duration =
            std::chrono::seconds(1)) {
    if(thread_count <= 0) {
        throw std::invalid_argument("Thread count must be positive");
    }
    if(sample_duration.count() <= 0) {
        throw std::invalid_argument(
            "CPU sampling duration must be positive");
    }

    // the calling thread's CPUs as they were on the first call: a program
    // whose main thread is also a worker pins it to one CPU, which would leave
    // a later call, for another thread count, only that one
    static const cpu_set_t allowed = [] {
        cpu_set_t set;
        CPU_ZERO(&set);
        if(sched_getaffinity(0, sizeof(set), &set) != 0) {
            int error = errno;
            throw std::system_error(
                error, std::generic_category(), "sched_getaffinity failed");
        }
        return set;
    }();

    std::map<int, detail::CpuTimes> before = detail::read_cpu_times();
    std::this_thread::sleep_for(sample_duration);
    std::map<int, detail::CpuTimes> after = detail::read_cpu_times();
    std::map<int, int> numa_nodes = detail::read_numa_nodes();

    std::vector<detail::CpuCandidate> candidates;
    for(const auto& entry: after) {
        int cpu = entry.first;
        if(cpu < 0 || cpu >= CPU_SETSIZE || !CPU_ISSET(cpu, &allowed)) {
            continue;
        }

        auto previous = before.find(cpu);
        if(previous == before.end()) continue;
        if(entry.second.total <= previous->second.total ||
                entry.second.idle < previous->second.idle) {
            continue;
        }

        std::uint64_t total_delta =
            entry.second.total - previous->second.total;
        std::uint64_t idle_delta =
            entry.second.idle - previous->second.idle;

        auto node = numa_nodes.find(cpu);
        candidates.push_back({
            cpu,
            node == numa_nodes.end() ? -1 : node->second,
            detail::read_core_key(cpu),
            static_cast<double>(idle_delta) / total_delta
        });
    }

    if(candidates.size() < static_cast<std::size_t>(thread_count)) {
        throw std::runtime_error(
            "Not enough allowed CPUs for the requested thread count");
    }

    std::map<int, std::vector<detail::CpuCandidate>> candidates_by_node;
    for(const detail::CpuCandidate& candidate: candidates) {
        candidates_by_node[candidate.numa_node].push_back(candidate);
    }

    if(spread_threads()) {
        std::vector<int> cpu_ids =
            detail::spread_over_nodes(candidates_by_node, thread_count);
        if(cpu_ids.empty()) {
            throw std::runtime_error(
                "Not enough allowed CPUs on each node to spread the threads");
        }
        return cpu_ids;
    }

    std::vector<detail::CpuCandidate> selected;
    double best_score = -1.0;
    for(const auto& node: candidates_by_node) {
        if(node.second.size() <
                static_cast<std::size_t>(thread_count)) {
            continue;
        }

        std::vector<detail::CpuCandidate> node_selection =
            detail::choose_candidates(node.second, thread_count);
        if(node_selection.size() !=
                static_cast<std::size_t>(thread_count)) {
            continue;
        }

        double score = 0.0;
        for(const detail::CpuCandidate& candidate: node_selection) {
            score += candidate.idle_ratio;
        }
        if(score > best_score) {
            best_score = score;
            selected = std::move(node_selection);
        }
    }

    if(selected.empty()) {
        selected = detail::choose_candidates(candidates, thread_count);
    }
    if(selected.size() != static_cast<std::size_t>(thread_count)) {
        throw std::runtime_error("Failed to select enough CPUs");
    }

    std::vector<int> cpu_ids;
    for(const detail::CpuCandidate& candidate: selected) {
        cpu_ids.push_back(candidate.cpu);
    }
    return cpu_ids;
}

// a program's thread counts from its argument: one count, or several
// separated by commas, run in that order on one load of the graph
inline std::vector<int> parse_thread_counts(const std::string& argument) {
    std::vector<int> counts;
    std::stringstream items(argument);
    std::string item;
    while(std::getline(items, item, ',')) {
        std::size_t used = 0;
        int count = 0;
        try {
            count = std::stoi(item, &used);
        }
        catch(const std::exception&) {
            used = 0;
        }
        if(used == 0 || used != item.size() || count <= 0) {
            throw std::invalid_argument(
                "Thread counts must be positive integers separated by commas");
        }
        counts.push_back(count);
    }
    if(counts.empty()) {
        throw std::invalid_argument("No thread count given");
    }
    return counts;
}

// how long a program given several thread counts rests between them, besides
// the second select_idle_cpus spends sampling
constexpr std::chrono::seconds pause_between_thread_counts{2};

inline void pin_current_thread(int cpu) {
    if(cpu < 0 || cpu >= CPU_SETSIZE) {
        throw std::out_of_range("CPU ID is outside CPU_SETSIZE");
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int error = pthread_setaffinity_np(
        pthread_self(), sizeof(set), &set);
    if(error != 0) {
        throw std::system_error(
            error, std::generic_category(),
            "pthread_setaffinity_np failed");
    }
}

} // namespace cpu_helpers

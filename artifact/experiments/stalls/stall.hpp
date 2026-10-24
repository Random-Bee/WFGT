// Stalled threads, for the experiments of stalls.sh and noise.sh: the stall
// builds here and in ../helping and ../work include this file, and stall their
// workers in three ways, each set from the environment and off when unset:
//   - per level: each of the last BFS_STALLED workers busy-waits BFS_STALL_US
//     microseconds at the first stall point it reaches in every level or
//     round, as a thread delayed while it holds part of that level's work would
//   - at random times: every worker, on its own, runs for an exponentially
//     distributed time of mean BFS_NOISE_GAP_US microseconds, is then stopped
//     by a signal wherever it is, and busy-waits BFS_NOISE_US microseconds in
//     the handler before it runs on; BFS_NOISE_SEED seeds the times
//   - at chosen points of our methods: a worker stalls with probability
//     BFS_TARGET_PPB per billion at each point of the kind BFS_TARGET names,
//     busy-waiting BFS_TARGET_US microseconds. The kinds are cas, right after
//     a discovery's winning compare-and-swap, and hub, halfway through the
//     neighbors of a vertex with at least BFS_HUB_DEGREE of them (1024 when
//     unset). Points of both kinds are counted, so a run can report how many
//     a search passes
// They also report what the system itself took from the workers (sched_watch,
// and for our programs search_started and search_returned), for runs in which
// the workers take every core of a socket. The benchmarked programs do not
// include this file
#pragma once

#include <sched.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace stalls {

// a whole number from 0 to `most` from the environment, 0 if unset; anything
// else ends the program, since a run with a misread setting would be wrong
inline long long setting(const char* name, long long most) {
    const char* text = std::getenv(name);
    if(text == nullptr || *text == '\0') return 0;
    char* end = nullptr;
    errno = 0;
    long long value = std::strtoll(text, &end, 10);
    if(errno != 0 || *end != '\0' || value < 0 || value > most) {
        std::fprintf(stderr, "Error: %s must be a whole number from 0 to %lld\n", name, most);
        std::exit(2);
    }
    return value;
}

inline long long stalled_workers() {
    static const long long count = setting("BFS_STALLED", INT_MAX);
    return count;
}

inline long long stall_us() {
    static const long long us = setting("BFS_STALL_US", 60'000'000);
    return us;
}

// true at the first stall point this thread reaches in a level or round, so
// `round` must change from one level or round to the next
inline bool first_in(long long round) {
    thread_local long long last_round = -1;
    if(round == last_round) return false;
    last_round = round;
    return true;
}

inline void wait_if_stalled(long long worker, long long workers) {
    if(worker < workers - stalled_workers() || stall_us() == 0) return;
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(stall_us());
    while(std::chrono::steady_clock::now() < until) {}
}

// the calling thread's id, as signals and affinities name it
inline pid_t current_thread() {
    return static_cast<pid_t>(syscall(SYS_gettid));
}

inline long long monotonic_ns() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1'000'000'000LL + now.tv_nsec;
}

// only reads the clock, which a signal handler may do
inline void spin_ns(long long ns) {
    long long until = monotonic_ns() + ns;
    while(monotonic_ns() < until) {}
}

inline long long noise_us() {
    static const long long us = setting("BFS_NOISE_US", 60'000'000);
    return us;
}

inline long long noise_gap_us() {
    static const long long us = setting("BFS_NOISE_GAP_US", 60'000'000);
    return us;
}

inline long long noise_seed() {
    static const long long seed = setting("BFS_NOISE_SEED", LLONG_MAX);
    return seed;
}

// set before the handler is installed, since a handler may not run the
// initialization of a function's static
inline std::atomic<long long> noise_ns{0};

inline void noise_handler(int) {
    int saved = errno;
    spin_ns(noise_ns.load(std::memory_order_relaxed));
    errno = saved;
}

// read before any thread is pinned
inline const cpu_set_t process_cpus = [] {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    if(sched_getaffinity(0, sizeof cpus, &cpus) != 0) CPU_ZERO(&cpus);
    return cpus;
}();

// stalls the given threads at random times (see the top) from when it is made
// until stop. A thread of its own sends the signals, from a CPU none of them
// is pinned to, so that it takes no time from them; it moves there itself,
// since a thread starts pinned where the thread that made it is, and in some
// programs that is a worker
class noise {
public:
    explicit noise(std::vector<pid_t> workers) {
        if(noise_us() == 0 || workers.empty()) return;
        noise_ns.store(noise_us() * 1000, std::memory_order_relaxed);
        struct sigaction action;
        std::memset(&action, 0, sizeof action);
        action.sa_handler = noise_handler;
        sigemptyset(&action.sa_mask);
        // a worker stopped in a system call, such as an idle one asleep, goes
        // back to it after the stall
        action.sa_flags = SA_RESTART;
        if(sigaction(SIGUSR1, &action, nullptr) != 0) {
            std::perror("Error: sigaction");
            std::exit(2);
        }
        injector = std::thread([this, workers = std::move(workers)] { inject(workers); });
    }

    noise(const noise&) = delete;
    noise& operator=(const noise&) = delete;

    ~noise() { stop(); }

    // a stall already begun still runs to its end
    void stop() {
        if(!injector.joinable()) return;
        stopping.store(true);
        injector.join();
    }

private:
    std::atomic<bool> stopping{false};
    std::thread injector;

    void inject(const std::vector<pid_t>& workers) {
        cpu_set_t spare = process_cpus;
        for(pid_t worker: workers) {
            cpu_set_t taken;
            if(sched_getaffinity(worker, sizeof taken, &taken) != 0) continue;
            for(int cpu=0; cpu<CPU_SETSIZE; cpu++) {
                if(CPU_ISSET(cpu, &taken)) CPU_CLR(cpu, &spare);
            }
        }
        if(CPU_COUNT(&spare) > 0) sched_setaffinity(0, sizeof spare, &spare);
        // on time, rather than up to the default 50 us late
        prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);

        const long long stall = noise_us() * 1000;
        std::mt19937_64 rng(static_cast<std::uint64_t>(noise_seed()));
        std::exponential_distribution<double> gap(
            1.0 / static_cast<double>(std::max(1LL, noise_gap_us() * 1000)));
        // when each worker is next stopped
        std::vector<long long> next(workers.size());
        const long long start = monotonic_ns();
        for(long long& time: next) time = start + static_cast<long long>(gap(rng));
        const pid_t process = getpid();

        while(!stopping.load()) {
            size_t i = std::min_element(next.begin(), next.end()) - next.begin();
            // a millisecond at most, so that a stop is seen soon
            long long wake = std::min(next[i], monotonic_ns() + 1'000'000);
            timespec until;
            until.tv_sec = static_cast<time_t>(wake / 1'000'000'000);
            until.tv_nsec = static_cast<long>(wake % 1'000'000'000);
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr);
            if(monotonic_ns() < next[i]) continue;
            syscall(SYS_tgkill, process, workers[i], SIGUSR1);
            // its next run starts once this stall ends
            next[i] += stall + static_cast<long long>(gap(rng));
        }
    }
};

enum target_kind { target_cas, target_hub, target_kinds };

// the kind stalled at, or target_kinds for none
inline int target() {
    static const int kind = [] {
        const char* text = std::getenv("BFS_TARGET");
        if(text == nullptr || *text == '\0') return static_cast<int>(target_kinds);
        if(std::strcmp(text, "cas") == 0) return static_cast<int>(target_cas);
        if(std::strcmp(text, "hub") == 0) return static_cast<int>(target_hub);
        std::fprintf(stderr, "Error: BFS_TARGET must be cas or hub\n");
        std::exit(2);
    }();
    return kind;
}

inline long long target_us() {
    static const long long us = setting("BFS_TARGET_US", 60'000'000);
    return us;
}

inline long long target_ppb() {
    static const long long ppb = setting("BFS_TARGET_PPB", 1'000'000'000);
    return ppb;
}

inline long long hub_degree() {
    static const long long degree = [] {
        long long value = setting("BFS_HUB_DEGREE", INT_MAX);
        return value > 0 ? value : 1024;
    }();
    return degree;
}

// points passed, by this thread since it last added them to the totals
inline thread_local long long passed_here[target_kinds];
inline std::atomic<long long> passed_total[target_kinds];

inline void at_target(int kind) {
    passed_here[kind]++;
    if(kind != target() || target_ppb() == 0) return;
    thread_local std::mt19937_64 rng(static_cast<std::uint64_t>(noise_seed()) ^
                                     static_cast<std::uint64_t>(current_thread()));
    if(rng() % 1'000'000'000 < static_cast<std::uint64_t>(target_ppb())) {
        spin_ns(target_us() * 1000);
    }
}

inline long long thread_cpu_ns() {
    timespec now;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return now.tv_sec * 1'000'000'000LL + now.tv_nsec;
}

// for our programs, whose workers spin rather than sleep: how long a worker was
// off its core from when it starts a search until it returns, which is the
// time that passed less the time it ran, and so counts interrupts too
inline thread_local long long search_wall_start, search_cpu_start, off_cpu_here;
inline std::atomic<long long> off_cpu_total{0};
// the most of one worker in one search
inline std::atomic<long long> off_cpu_most{0};

inline void search_started() {
    search_wall_start = monotonic_ns();
    search_cpu_start = thread_cpu_ns();
}

inline void search_returned() {
    long long off = (monotonic_ns() - search_wall_start) - (thread_cpu_ns() - search_cpu_start);
    if(off < 0) off = 0;
    off_cpu_here += off;
    long long most = off_cpu_most.load();
    while(off > most && !off_cpu_most.compare_exchange_weak(most, off)) {}
}

// a worker adds its counts after every search, rather than at every point,
// where so many threads adding to one total would wait for each other
inline void add_passed() {
    for(int kind=0; kind<target_kinds; kind++) {
        passed_total[kind].fetch_add(passed_here[kind]);
        passed_here[kind] = 0;
    }
    off_cpu_total.fetch_add(off_cpu_here);
    off_cpu_here = 0;
}

inline long long passed(int kind) {
    return passed_total[kind].load();
}

inline void reset_passed() {
    for(std::atomic<long long>& total: passed_total) total.store(0);
    off_cpu_total.store(0);
    off_cpu_most.store(0);
}

// what a worker got from the system: how long it was ready to run but waited
// for a core (its run delay), how often it gave up its core itself, as a
// worker does that sleeps when it has no work, and how often the system took
// the core away
struct sched_counts {
    long long run_delay_ns = 0;
    long long voluntary = 0;
    long long involuntary = 0;
};

inline sched_counts read_sched(pid_t thread) {
    sched_counts counts;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/self/task/%d/schedstat", static_cast<int>(thread));
    if(std::FILE* file = std::fopen(path, "r")) {
        long long ran_ns = 0;
        if(std::fscanf(file, "%lld %lld", &ran_ns, &counts.run_delay_ns) != 2) {
            counts.run_delay_ns = 0;
        }
        std::fclose(file);
    }
    std::snprintf(path, sizeof path, "/proc/self/task/%d/status", static_cast<int>(thread));
    if(std::FILE* file = std::fopen(path, "r")) {
        char line[256];
        while(std::fgets(line, sizeof line, file) != nullptr) {
            std::sscanf(line, "voluntary_ctxt_switches: %lld", &counts.voluntary);
            std::sscanf(line, "nonvoluntary_ctxt_switches: %lld", &counts.involuntary);
        }
        std::fclose(file);
    }
    return counts;
}

// reads the workers' counts when made, and again in report, which prints what
// they gained per worker and search: the mean of each count over the workers,
// and the most run delay of one. The workers must still be running then
class sched_watch {
public:
    explicit sched_watch(std::vector<pid_t> workers) : workers(std::move(workers)) {
        for(pid_t worker: this->workers) start.push_back(read_sched(worker));
    }

    void report(long long searches) const {
        if(workers.empty() || searches <= 0) return;
        double per_search = 1.0 / static_cast<double>(searches);
        double delay = 0, most_delay = 0, voluntary = 0, involuntary = 0;
        for(size_t i=0; i<workers.size(); i++) {
            sched_counts now = read_sched(workers[i]);
            double worker_delay =
                static_cast<double>(now.run_delay_ns - start[i].run_delay_ns) / 1e3 * per_search;
            delay += worker_delay;
            most_delay = std::max(most_delay, worker_delay);
            voluntary += static_cast<double>(now.voluntary - start[i].voluntary) * per_search;
            involuntary += static_cast<double>(now.involuntary - start[i].involuntary) * per_search;
        }
        double n = static_cast<double>(workers.size());
        std::fprintf(stderr,
                     "sched run_delay_us %.1f most_run_delay_us %.1f voluntary %.2f involuntary %.2f\n",
                     delay / n, most_delay, voluntary / n, involuntary / n);
    }

private:
    std::vector<pid_t> workers;
    std::vector<sched_counts> start;
};

// each search's time in microseconds, printed after a thread count's
// searches, so that the first can be told from the others
class search_times {
public:
    void add(long long us) { times.push_back(us); }

    void report() const {
        std::fprintf(stderr, "searches_us");
        for(long long us: times) std::fprintf(stderr, " %lld", us);
        std::fprintf(stderr, "\n");
    }

private:
    std::vector<long long> times;
};

}  // namespace stalls

// worker and workers are evaluated only at a round's first stall point, since
// some stall points are passed once per vertex or edge
#define STALL_POINT(worker, workers, round) \
    do { \
        if(stalls::first_in(round)) stalls::wait_if_stalled((worker), (workers)); \
    } while(0)
#define STALL_TARGET(kind) stalls::at_target(stalls::target_##kind)
// at the middle of a vertex's neighbors, or of the blocks they are walked in
#define STALL_HALFWAY(position, total, degree) \
    do { \
        if((position) == (total) / 2 && \
           static_cast<long long>(degree) >= stalls::hub_degree()) { \
            STALL_TARGET(hub); \
        } \
    } while(0)

// Cross-process ping-pong latency: shm ring vs pipe vs Unix domain socket vs eventfd.
//
//   ipc_latency --transport shm|pipe|uds|eventfd [--spin] [--cpus A,B]
//               [--iters N] [--warmup N] [--huge]
//
// --huge: back the shm rings with 2 MiB hugetlbfs pages (needs vm.nr_hugepages > 0).
//
// The parent (pinned to A) sends an 8-byte message, the child (pinned to B)
// echoes it, and the parent stamps the TSC on either side of the round trip.
// One-way latency is reported as RTT/2, so every transport is measured the
// same way and no cross-core TSC comparison is needed.
//
// Blocking vs --spin: by default pipe/uds/eventfd do a blocking read(), which
// is how they are normally used and includes the scheduler wake-up. --spin
// makes the read end O_NONBLOCK and busy-polls, which removes the wake-up and
// leaves only the syscall + kernel copy. The shm ring always spins.
//
// Output: one CSV line to stdout (plus a human summary on stderr).

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "shmring/shmring.hpp"

using namespace shmring;

namespace {

struct Msg {
    std::uint64_t seq;
};

using LatRing = SpscRing<Msg, 1024>;

struct Opts {
    std::string transport = "shm";
    bool spin = false;
    int cpu_a = 1, cpu_b = 3;
    std::size_t iters = 1'000'000, warmup = 100'000;
    bool huge = false;
};

[[noreturn]] void die(const char* what) {
    std::perror(what);
    std::exit(1);
}

// Byte-stream helpers: an 8-byte message may in principle arrive in pieces.
void write_all(int fd, const void* p, std::size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
        const ssize_t w = ::write(fd, c, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            die("write");
        }
        c += w;
        n -= std::size_t(w);
    }
}
void read_all(int fd, void* p, std::size_t n) {
    char* c = static_cast<char*>(p);
    while (n) {
        const ssize_t r = ::read(fd, c, n);
        if (r < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;  // EAGAIN = --spin mode, poll again
            die("read");
        }
        if (r == 0) std::exit(0);  // peer gone
        c += r;
        n -= std::size_t(r);
    }
}

void set_nonblock(int fd) {
    const int fl = ::fcntl(fd, F_GETFL);
    if (::fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) die("fcntl");
}

// A transport is two half-duplex endpoints: a->b and b->a.
struct FdPair {
    int a_send, a_recv, b_send, b_recv;
};

FdPair make_fds(const std::string& t) {
    FdPair p{};
    if (t == "pipe") {
        int ab[2], ba[2];
        if (::pipe(ab) || ::pipe(ba)) die("pipe");
        p = {ab[1], ba[0], ba[1], ab[0]};
    } else if (t == "uds") {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) die("socketpair");
        p = {sv[0], sv[0], sv[1], sv[1]};
    } else if (t == "eventfd") {
        // eventfd carries a counter, not bytes: each "message" is write(1)/read().
        const int ab = ::eventfd(0, 0), ba = ::eventfd(0, 0);
        if (ab < 0 || ba < 0) die("eventfd");
        p = {ab, ba, ba, ab};
    } else {
        std::fprintf(stderr, "unknown transport %s\n", t.c_str());
        std::exit(2);
    }
    return p;
}

void fd_send(int fd, std::uint64_t v) { write_all(fd, &v, sizeof v); }
std::uint64_t fd_recv(int fd) {
    std::uint64_t v;
    read_all(fd, &v, sizeof v);
    return v;
}

double pct(const std::vector<std::uint64_t>& s, double p) {
    const std::size_t i = std::min(s.size() - 1, std::size_t(p * double(s.size())));
    return double(s[i]);
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--transport") o.transport = next();
        else if (a == "--spin") o.spin = true;
        else if (a == "--cpus") {
            const std::string v = next();
            std::sscanf(v.c_str(), "%d,%d", &o.cpu_a, &o.cpu_b);
        } else if (a == "--iters") o.iters = std::stoull(next());
        else if (a == "--warmup") o.warmup = std::stoull(next());
        else if (a == "--huge") o.huge = true;
        else {
            std::fprintf(stderr, "usage: %s --transport shm|pipe|uds|eventfd [--spin] [--cpus A,B] [--iters N]\n",
                         argv[0]);
            return 2;
        }
    }

    const double tpn = tsc_per_ns();
    const std::size_t total = o.warmup + o.iters;
    std::vector<std::uint64_t> rtt;
    rtt.reserve(o.iters);

    const bool shm = o.transport == "shm";
    const std::string ping_name = "shmring_lat_ping_" + std::to_string(::getpid());
    const std::string pong_name = "shmring_lat_pong_" + std::to_string(::getpid());
    Channel<LatRing> ping, pong;
    FdPair fds{};
    if (shm) {
        const HugePages hp = o.huge ? HugePages::Require : HugePages::No;
        ping = Channel<LatRing>::create(ping_name, hp);
        pong = Channel<LatRing>::create(pong_name, hp);
    } else {
        fds = make_fds(o.transport);
    }

    const pid_t child = ::fork();
    if (child < 0) die("fork");

    if (child == 0) {  // ------------------------------------------------ echo side
        if (!pin_to_cpu(o.cpu_b)) die("pin child");
        if (shm) {
            // Attach by name rather than use the inherited mapping, so this is
            // the same code path an unrelated process would take.
            auto in = Channel<LatRing>::attach(ping_name);
            auto out = Channel<LatRing>::attach(pong_name);
            Msg m;
            for (std::size_t i = 0; i < total; ++i) {
                while (!in->try_pop(m)) cpu_relax();
                while (!out->try_push(m)) cpu_relax();
            }
        } else {
            if (o.spin) set_nonblock(fds.b_recv);
            for (std::size_t i = 0; i < total; ++i) fd_send(fds.b_send, fd_recv(fds.b_recv));
        }
        std::_Exit(0);
    }

    // ------------------------------------------------------------- timing side
    if (!pin_to_cpu(o.cpu_a)) die("pin parent");
    if (!shm && o.spin) set_nonblock(fds.a_recv);
    std::uint64_t check = 0;
    for (std::size_t i = 0; i < total; ++i) {
        const std::uint64_t t0 = rdtsc_fenced();
        if (shm) {
            ping->push(Msg{i});
            Msg m;
            while (!pong->try_pop(m)) cpu_relax();
            check += m.seq;
        } else {
            fd_send(fds.a_send, i + 1);  // eventfd rejects writing 0
            check += fd_recv(fds.a_recv);
        }
        const std::uint64_t t1 = rdtsc_fenced();
        if (i >= o.warmup) rtt.push_back(t1 - t0);
    }
    int status = 0;
    ::waitpid(child, &status, 0);

    // eventfd may coalesce counts only if two writes land before one read;
    // ping-pong never has two in flight, so the echo must be exact.
    const std::uint64_t expect = shm ? (total - 1) * total / 2 : total * (total + 1) / 2;
    if (check != expect) {
        std::fprintf(stderr, "echo mismatch: %llu vs %llu\n", (unsigned long long)check,
                     (unsigned long long)expect);
        return 1;
    }

    std::sort(rtt.begin(), rtt.end());
    auto ow = [&](double p) { return pct(rtt, p) / tpn / 2.0; };  // one-way ns
    double mean = 0;
    for (auto v : rtt) mean += double(v);
    mean = mean / double(rtt.size()) / tpn / 2.0;

    const std::string name = o.transport + (shm ? (o.huge ? "-huge" : "") : (o.spin ? "-spin" : "-block"));
    if (shm)
        std::fprintf(stderr, "shm page size: %zu kB\n", ping.segment().page_size_kb());
    std::fprintf(stderr, "%-14s cpus %d,%d  one-way ns: min %.0f  p50 %.0f  p99 %.0f  p99.9 %.0f  max %.0f  mean %.0f\n",
                 name.c_str(), o.cpu_a, o.cpu_b, double(rtt.front()) / tpn / 2, ow(0.50), ow(0.99), ow(0.999),
                 double(rtt.back()) / tpn / 2, mean);
    std::printf("%s,%d,%d,%zu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", name.c_str(), o.cpu_a, o.cpu_b, rtt.size(),
                double(rtt.front()) / tpn / 2, ow(0.50), ow(0.99), ow(0.999), double(rtt.back()) / tpn / 2, mean);
    return 0;
}

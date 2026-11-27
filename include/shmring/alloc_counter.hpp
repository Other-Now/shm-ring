#pragma once

// Counts global operator new calls, so a binary can assert "zero allocations
// in steady state" about itself without an external tool.
//
// Include this header everywhere you want to read the counter. In exactly ONE
// translation unit, define SHMRING_COUNT_ALLOCS before including it: that TU
// then also provides the replacement operator new/delete.
//
// This only sees C++ allocations. malloc() called directly (from C code or the
// C library) is invisible here; that is what the Valgrind run is for.
//
// Finding the culprit: call alloc_trace_arm(true) where steady state begins
// and every later operator new prints a backtrace to stderr (link with
// -rdynamic for symbol names). This is how the risk gate's per-order
// allocations and the book's level-vector growth were located.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace shmring {

inline std::atomic<std::uint64_t>& alloc_counter() noexcept {
    static std::atomic<std::uint64_t> n{0};
    return n;
}
inline std::uint64_t alloc_count() noexcept { return alloc_counter().load(std::memory_order_relaxed); }

inline std::atomic<bool>& alloc_trace_flag() noexcept {
    static std::atomic<bool> armed{false};
    return armed;
}
inline void alloc_trace_arm(bool on) noexcept { alloc_trace_flag().store(on, std::memory_order_relaxed); }

}  // namespace shmring

#ifdef SHMRING_COUNT_ALLOCS
#include <cstdlib>
#include <new>

#include <execinfo.h>
#include <unistd.h>

namespace shmring::detail {
inline void on_alloc(std::size_t n) noexcept {
    alloc_counter().fetch_add(1, std::memory_order_relaxed);
    if (alloc_trace_flag().load(std::memory_order_relaxed)) {
        alloc_trace_arm(false);  // backtrace() itself may allocate; don't recurse
        char head[64];
        const int len = __builtin_snprintf(head, sizeof head, "steady-state operator new(%zu):\n", n);
        (void)!::write(2, head, std::size_t(len));
        void* frames[24];
        const int k = ::backtrace(frames, 24);
        ::backtrace_symbols_fd(frames, k, 2);
        alloc_trace_arm(true);
    }
}
}  // namespace shmring::detail

// GCC cannot see that these replace the global operators and warns about
// malloc/free pairing; the pairing is correct by construction.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"

void* operator new(std::size_t n) {
    shmring::detail::on_alloc(n);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    shmring::detail::on_alloc(n);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t n, std::align_val_t a) {
    shmring::detail::on_alloc(n);
    const std::size_t al = static_cast<std::size_t>(a);
    if (void* p = std::aligned_alloc(al, (n + al - 1) / al * al)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
#pragma GCC diagnostic pop
#endif

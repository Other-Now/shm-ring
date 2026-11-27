#pragma once

// Fixed-capacity object pool: all storage is reserved when the pool is built,
// objects are constructed into it with placement new and destroyed in place.
// After construction acquire()/release() never touch the heap, which is what
// lets the hot path run with zero allocations (verified by the allocation
// counter in tests and by Valgrind in tools/valgrind_steady_state.sh).
//
// Single-threaded on purpose: one pool per process / per thread. A free list of
// indices (a stack) gives LIFO reuse, so the most recently released -- and
// therefore cache-hot -- object is the next one handed out.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace shmring {

template <class T, std::size_t N>
class ObjectPool {
    static_assert(N > 0 && N <= UINT32_MAX);

public:
    ObjectPool() noexcept {
        for (std::size_t i = 0; i < N; ++i) free_[i] = std::uint32_t(N - 1 - i);
        top_ = N;
    }
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ~ObjectPool() = default;  // objects still out are the caller's leak, as with new/delete

    // nullptr when exhausted -- the caller decides whether that is a reject or a bug.
    template <class... Args>
    T* acquire(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        if (top_ == 0) return nullptr;
        const std::uint32_t i = free_[--top_];
        return ::new (static_cast<void*>(&storage_[i])) T(std::forward<Args>(args)...);
    }

    void release(T* p) noexcept {
        p->~T();
        free_[top_++] = index_of(p);
    }

    struct Releaser {
        ObjectPool* pool;
        void operator()(T* p) const noexcept { pool->release(p); }
    };
    using Handle = std::unique_ptr<T, Releaser>;

    template <class... Args>
    Handle make(Args&&... args) {
        return Handle(acquire(std::forward<Args>(args)...), Releaser{this});
    }

    std::size_t available() const noexcept { return top_; }
    std::size_t in_use() const noexcept { return N - top_; }
    static constexpr std::size_t capacity() noexcept { return N; }

    bool owns(const T* p) const noexcept {
        const auto* b = reinterpret_cast<const Slot*>(p);
        return b >= storage_ && b < storage_ + N;
    }

private:
    struct alignas(T) Slot {
        std::byte bytes[sizeof(T)];
    };

    std::uint32_t index_of(const T* p) const noexcept {
        return std::uint32_t(reinterpret_cast<const Slot*>(p) - storage_);
    }

    Slot storage_[N];
    std::uint32_t free_[N];
    std::size_t top_;
};

}  // namespace shmring

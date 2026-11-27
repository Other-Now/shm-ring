#pragma once

// Putting a typed object into a named segment, and picking the queue type at
// compile time.
//
//   Shared<Obj>::create(name)  creator: map, placement-new Obj, publish header
//   Shared<Obj>::attach(name)  attacher: map, wait for header, validate
//
// The header is published last with a release store of `magic`, and the
// attacher spins on an acquire load of it, so an attacher can never see a
// half-constructed ring. The header also records sizeof/alignof and a type tag,
// so attaching a SpscRing<Foo,1024> to a segment holding SpscRing<Bar,4096>
// fails loudly instead of corrupting memory.
//
// Channel selection:
//   Ring<T, N, Producers::One>   -> SpscRing<T, N>
//   Ring<T, N, Producers::Many>  -> MpscRing<T, N>
// Both satisfy the Queue concept, so code written as `template <Queue Q>` gets
// the concrete type with no virtual dispatch.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#include "shmring/concepts.hpp"
#include "shmring/mpsc_ring.hpp"
#include "shmring/shm_segment.hpp"
#include "shmring/spsc_ring.hpp"

namespace shmring {

enum class Producers { One, Many };

template <ShmPayload T, std::size_t N, Producers P = Producers::One>
using Ring = std::conditional_t<P == Producers::One, SpscRing<T, N>, MpscRing<T, N>>;

namespace detail {

// FNV-1a over the compiler's spelling of the type. Identical binaries (or
// binaries from the same compiler) agree; that is the case this guards.
template <class T>
constexpr std::uint64_t type_tag() {
    constexpr std::string_view s = __PRETTY_FUNCTION__;
    std::uint64_t h = 1469598103934665603ull;
    for (char c : s) h = (h ^ std::uint8_t(c)) * 1099511628211ull;
    return h;
}

struct alignas(kCacheLine) Header {
    std::atomic<std::uint64_t> magic;  // written last
    std::uint64_t tag;
    std::uint64_t size;
    std::uint64_t align;
    std::uint32_t version;
};

inline constexpr std::uint64_t kMagic = 0x474e495224d4853ull;  // arbitrary, just not zero
inline constexpr std::uint32_t kVersion = 1;

}  // namespace detail

template <ShmPlaceable Obj>
class Shared {
    static constexpr std::size_t kOffset =
        (sizeof(detail::Header) + alignof(Obj) - 1) / alignof(Obj) * alignof(Obj);

public:
    static Shared create(const std::string& name, HugePages hp = HugePages::No) {
        Shared s;
        s.seg_ = ShmSegment::create(name, kOffset + sizeof(Obj), hp);
        auto* h = ::new (s.seg_.data()) detail::Header{};
        s.obj_ = ::new (static_cast<char*>(s.seg_.data()) + kOffset) Obj();
        h->tag = detail::type_tag<Obj>();
        h->size = sizeof(Obj);
        h->align = alignof(Obj);
        h->version = detail::kVersion;
        h->magic.store(detail::kMagic, std::memory_order_release);
        return s;
    }

    // Waits up to `timeout` for the creator. Throws on timeout or mismatch.
    static Shared attach(const std::string& name,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        Shared s;
        for (;;) {
            s.seg_ = ShmSegment::open(name);
            if (s.seg_.valid() && s.seg_.size() >= kOffset + sizeof(Obj)) {
                auto* h = static_cast<detail::Header*>(s.seg_.data());
                if (h->magic.load(std::memory_order_acquire) == detail::kMagic) {
                    if (h->version != detail::kVersion || h->size != sizeof(Obj) || h->align != alignof(Obj) ||
                        h->tag != detail::type_tag<Obj>())
                        throw std::runtime_error("shm-ring: segment '" + name + "' holds a different type");
                    s.obj_ = std::launder(reinterpret_cast<Obj*>(static_cast<char*>(s.seg_.data()) + kOffset));
                    return s;
                }
            }
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("shm-ring: timed out attaching to '" + name + "'");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Obj* get() const noexcept { return obj_; }
    Obj* operator->() const noexcept { return obj_; }
    Obj& operator*() const noexcept { return *obj_; }
    const ShmSegment& segment() const noexcept { return seg_; }

private:
    ShmSegment seg_;
    Obj* obj_ = nullptr;
};

// A Channel is a Shared queue; the alias exists so call sites read as intent.
template <Queue Q>
using Channel = Shared<Q>;

}  // namespace shmring

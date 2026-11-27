#pragma once

// The compile-time contracts.
//
// ShmPayload: what may be copied through shared memory. Trivially copyable
// (the ring moves bytes, it never runs a constructor in the other process) and
// standard layout (both processes must agree on the byte layout). This cannot
// catch a raw pointer member -- that is still the user's job -- but it does
// reject std::string, std::vector and anything virtual at compile time.
//
// Queue: what generic code may assume about a ring. Both SpscRing and MpscRing
// satisfy it, so code written against `Queue auto&` is instantiated for the
// concrete type and the abstraction compiles away (no vtable, no indirect
// call). bench/micro.cpp measures this against a virtual interface.

#include <concepts>
#include <cstddef>
#include <type_traits>

namespace shmring {

template <class T>
concept ShmPayload = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;

template <class Q>
concept Queue = requires(Q& q, const typename Q::value_type& in, typename Q::value_type& out) {
    typename Q::value_type;
    { Q::capacity } -> std::convertible_to<std::size_t>;
    { Q::multi_producer } -> std::convertible_to<bool>;
    { q.try_push(in) } -> std::same_as<bool>;
    { q.try_pop(out) } -> std::same_as<bool>;
} && ShmPayload<typename Q::value_type>;

// Anything placed in a segment: no process-local state may hide inside it.
template <class T>
concept ShmPlaceable = std::is_standard_layout_v<T> && std::is_default_constructible_v<T>;

}  // namespace shmring

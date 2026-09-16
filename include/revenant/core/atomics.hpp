#pragma once

#include <revenant/config.hpp>

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

// The only file allowed to name std::memory_order (scripts/lint.sh enforces it). Call sites
// spell the ordering in the function name, so every ordering decision is visible at the call
// site and the full set can be reviewed against docs/design.md in one place.

namespace revenant::core::atomics {

/// Shared-memory words: lock-free on every supported target (checked below).
template <typename T>
concept SharedWord = std::same_as<T, std::uint32_t> || std::same_as<T, std::uint64_t>;

namespace detail {

// A lock-free atomic load never writes memory, so loading through a const object is safe, even
// on a PROT_READ mapping. C++26's atomic_ref<const T> makes the const_cast unnecessary.
template <SharedWord T>
[[nodiscard]] std::atomic_ref<T> ref(const T& word) noexcept {
  static_assert(std::atomic_ref<T>::is_always_lock_free);
  return std::atomic_ref<T>{const_cast<T&>(word)};
}

}  // namespace detail

template <SharedWord T>
[[nodiscard]] T load_relaxed(const T& word) noexcept {
  return detail::ref(word).load(std::memory_order_relaxed);
}

template <SharedWord T>
[[nodiscard]] T load_acquire(const T& word) noexcept {
  return detail::ref(word).load(std::memory_order_acquire);
}

template <SharedWord T>
void store_relaxed(T& word, std::type_identity_t<T> value) noexcept {
  detail::ref(word).store(value, std::memory_order_relaxed);
}

template <SharedWord T>
void store_release(T& word, std::type_identity_t<T> value) noexcept {
  detail::ref(word).store(value, std::memory_order_release);
}

inline void fence_release() noexcept {
  std::atomic_thread_fence(std::memory_order_release);
}

inline void fence_acquire() noexcept {
  std::atomic_thread_fence(std::memory_order_acquire);
}

/// Copies `source` into shared memory as relaxed 8-byte stores, zero-padding the last word.
/// Narrow contract: `dest` is 8-byte aligned with room for `source` rounded up to 8 bytes.
inline void store_words_relaxed(std::byte* dest, std::span<const std::byte> source) noexcept {
  REVENANT_ASSERT(reinterpret_cast<std::uintptr_t>(dest) % alignof(std::uint64_t) == 0);
  for (std::size_t offset = 0; offset < source.size(); offset += sizeof(std::uint64_t)) {
    std::uint64_t word = 0;
    std::memcpy(&word, source.data() + offset, std::min(sizeof word, source.size() - offset));
    store_relaxed(*reinterpret_cast<std::uint64_t*>(dest + offset), word);
  }
}

/// Fills `dest` from shared memory with relaxed 8-byte loads; never writes past dest.size().
/// Narrow contract: `source` is 8-byte aligned with dest.size() rounded up to 8 bytes readable.
inline void load_words_relaxed(const std::byte* source, std::span<std::byte> dest) noexcept {
  REVENANT_ASSERT(reinterpret_cast<std::uintptr_t>(source) % alignof(std::uint64_t) == 0);
  for (std::size_t offset = 0; offset < dest.size(); offset += sizeof(std::uint64_t)) {
    const std::uint64_t word =
        load_relaxed(*reinterpret_cast<const std::uint64_t*>(source + offset));
    std::memcpy(dest.data() + offset, &word, std::min(sizeof word, dest.size() - offset));
  }
}

}  // namespace revenant::core::atomics

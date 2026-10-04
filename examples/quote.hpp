#pragma once

#include <cstdint>
#include <cstring>
#include <span>

// A normalised top-of-book quote: what a feed handler publishes and strategies consume.
struct Quote {
  std::uint64_t sent_ns;  ///< CLOCK_MONOTONIC at publish, comparable across processes.
  std::uint32_t symbol_id;
  std::uint32_t bid_size;
  std::int64_t bid_ticks;  ///< Price in integer ticks: never floating point on the wire.
  std::int64_t ask_ticks;
  std::uint32_t ask_size;
  std::uint32_t reserved;
};
static_assert(sizeof(Quote) == 40);

[[nodiscard]] inline std::span<const std::byte> as_bytes(const Quote& q) noexcept {
  return std::as_bytes(std::span{&q, 1});
}

[[nodiscard]] inline Quote quote_from(std::span<const std::byte> bytes) noexcept {
  Quote q{};
  std::memcpy(&q, bytes.data(), sizeof q);
  return q;
}

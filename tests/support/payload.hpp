#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Self-validating payloads. The length and every byte are derived from (seq, epoch), and the
// first 8 bytes are a tag unique to the message, so a flipped bit, a wrong length or bytes from
// any other message all fail check_payload. This is how stress and crash tests check INV4.

namespace revenant::testing {

inline constexpr std::size_t kMinPayload = 8;
inline constexpr std::size_t kMaxCapacity = 4080;

[[nodiscard]] constexpr std::uint64_t splitmix64(std::uint64_t& state) noexcept {
  std::uint64_t z = (state += 0x9e37'79b9'7f4a'7c15);
  z = (z ^ (z >> 30)) * 0xbf58'476d'1ce4'e5b9;
  z = (z ^ (z >> 27)) * 0x94d0'49bb'1331'11eb;
  return z ^ (z >> 31);
}

/// Writes message (seq, epoch)'s payload into `out` and returns its length, kMinPayload to
/// capacity. Narrow contract: kMinPayload <= capacity <= min(out.size(), kMaxCapacity).
inline std::size_t make_payload(std::uint64_t seq, std::uint32_t epoch, std::size_t capacity,
                                std::span<std::byte> out) noexcept {
  std::uint64_t state = seq ^ (std::uint64_t{epoch} * 0xd1b5'4a32'd192'ed03);
  const std::uint64_t tag = splitmix64(state);
  const std::size_t length = kMinPayload + tag % (capacity - kMinPayload + 1);

  std::memcpy(out.data(), &tag, sizeof tag);
  for (std::size_t offset = sizeof tag; offset < length; offset += sizeof(std::uint64_t)) {
    const std::uint64_t word = splitmix64(state);
    std::memcpy(out.data() + offset, &word, std::min(sizeof word, length - offset));
  }
  return length;
}

/// True iff `bytes` is exactly message (seq, epoch)'s payload for this capacity.
[[nodiscard]] inline bool check_payload(std::uint64_t seq, std::uint32_t epoch,
                                        std::size_t capacity,
                                        std::span<const std::byte> bytes) noexcept {
  std::array<std::byte, kMaxCapacity> expected{};
  const std::size_t length = make_payload(seq, epoch, capacity, expected);
  return bytes.size() == length && std::equal(bytes.begin(), bytes.end(), expected.begin());
}

}  // namespace revenant::testing

#pragma once

#include <revenant/config.hpp>

#include <bit>
#include <cstdint>

namespace revenant::core {

inline constexpr std::uint32_t kMinSlotSize = 64;
inline constexpr std::uint32_t kMaxSlotSize = 4096;
inline constexpr std::uint32_t kMinSlotCount = 2;
inline constexpr std::uint32_t kMaxSlotCount = std::uint32_t{1} << 24;

/// Shape of the slot ring: `slot_count` slots of `slot_size` bytes, both powers of two.
///
/// Sequence numbers are 64-bit and never wrap (2^63 messages at 10^9/s take ~292 years), so
/// mapping a sequence to its slot is a mask, and sequence comparisons are plain `<`.
/// Member functions other than `is_valid` have a narrow contract: the geometry must be valid.
struct RingGeometry {
  std::uint32_t slot_size;
  std::uint32_t slot_count;

  [[nodiscard]] static constexpr bool is_valid(std::uint32_t size, std::uint32_t count) noexcept {
    return std::has_single_bit(size) && size >= kMinSlotSize && size <= kMaxSlotSize &&
           std::has_single_bit(count) && count >= kMinSlotCount && count <= kMaxSlotCount;
  }

  [[nodiscard]] constexpr std::uint32_t mask() const noexcept {
    REVENANT_ASSERT(is_valid(slot_size, slot_count));
    return slot_count - 1;
  }

  /// Slot that holds sequence `seq`.
  [[nodiscard]] constexpr std::uint32_t index_of(std::uint64_t seq) const noexcept {
    // The mask is below 2^24, so the narrowing cannot lose bits.
    return static_cast<std::uint32_t>(seq & mask());
  }

  /// Byte offset of `seq`'s slot from the start of the slot array.
  [[nodiscard]] constexpr std::uint64_t slot_offset(std::uint64_t seq) const noexcept {
    return std::uint64_t{index_of(seq)} * slot_size;
  }

  /// Size of the slot array. At most 2^36 bytes, so it cannot overflow.
  [[nodiscard]] constexpr std::uint64_t slots_bytes() const noexcept {
    return std::uint64_t{slot_count} * slot_size;
  }

  friend constexpr bool operator==(const RingGeometry&, const RingGeometry&) noexcept = default;
};

}  // namespace revenant::core

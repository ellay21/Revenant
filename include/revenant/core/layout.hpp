#pragma once

#include <revenant/config.hpp>
#include <revenant/core/ring.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

// Wire format v1: byte-exact layout of a channel segment, shared between processes that may come
// from different builds. docs/wire-format.md is the normative description; changing any byte
// here requires bumping kWireVersion.
//
// The structs are implicit-lifetime types (trivially copyable aggregates) describing bytes in a
// shared mapping. Fields that change after initialisation are only ever accessed through
// std::atomic_ref; immutable header fields are read after an acquire load of `magic`.

namespace revenant::core {

inline constexpr std::uint64_t kMagic = 0x5245'564E'414E'5431;  // "REVNANT1"
inline constexpr std::uint32_t kWireVersion = 1;

/// Fixed wire constant; deliberately not std::hardware_destructive_interference_size, which can
/// vary with compiler flags and would silently change the layout.
inline constexpr std::size_t kWireLine = 64;

inline constexpr std::size_t kHeaderOffset = 0;
inline constexpr std::size_t kControlOffset = 64;
inline constexpr std::size_t kSlotsOffset = 256;
inline constexpr std::size_t kSlotHeaderSize = 16;

/// Largest sequence number; 2^63 messages at 10^9/s take ~292 years, so this is never reached.
inline constexpr std::uint64_t kMaxSequence = (std::uint64_t{1} << 63) - 1;

/// Offset 0. Immutable once `magic` is published.
struct SegmentHeader {
  std::uint64_t magic;  ///< 0 until initialised; kMagic is stored last, with release ordering.
  std::uint32_t wire_version;
  std::uint32_t slot_size;
  std::uint32_t slot_count;
  std::uint32_t reserved0;
  std::uint64_t layout_hash;
  std::uint64_t created_unix_ns;  ///< Diagnostic only.
  std::array<std::uint8_t, 24> reserved;
};

/// Offset 64. Each publisher-written field owns a full line, so a store to one never
/// invalidates the line holding the other.
struct ControlBlock {
  std::uint64_t head;  ///< Last sequence whose publication completed; 0 means none.
  std::array<std::uint8_t, kWireLine - sizeof(std::uint64_t)> pad_head;
  std::uint32_t epoch;  ///< Incremented by every publisher that acquires the lease.
  std::array<std::uint8_t, kWireLine - sizeof(std::uint32_t)> pad_epoch;
  std::array<std::uint8_t, kWireLine> reserved;
};

/// Start of every slot; the payload follows at offset 16.
struct SlotHeader {
  std::uint64_t seq_word;  ///< 0 never written, 2s-1 writing message s, 2s message s committed.
  std::uint64_t meta;      ///< (epoch << 32) | length; protected by the seqlock.
};

static_assert(sizeof(SegmentHeader) == kControlOffset - kHeaderOffset);
static_assert(offsetof(SegmentHeader, magic) == 0);
static_assert(offsetof(SegmentHeader, wire_version) == 8);
static_assert(offsetof(SegmentHeader, slot_size) == 12);
static_assert(offsetof(SegmentHeader, slot_count) == 16);
static_assert(offsetof(SegmentHeader, reserved0) == 20);
static_assert(offsetof(SegmentHeader, layout_hash) == 24);
static_assert(offsetof(SegmentHeader, created_unix_ns) == 32);
static_assert(offsetof(SegmentHeader, reserved) == 40);

static_assert(sizeof(ControlBlock) == kSlotsOffset - kControlOffset);
static_assert(offsetof(ControlBlock, head) == 0);
static_assert(offsetof(ControlBlock, epoch) == kWireLine);
static_assert(offsetof(ControlBlock, reserved) == 2 * kWireLine);

static_assert(sizeof(SlotHeader) == kSlotHeaderSize);
static_assert(offsetof(SlotHeader, seq_word) == 0);
static_assert(offsetof(SlotHeader, meta) == 8);

static_assert(std::is_standard_layout_v<SegmentHeader> &&
              std::is_trivially_copyable_v<SegmentHeader>);
static_assert(std::is_standard_layout_v<ControlBlock> &&
              std::is_trivially_copyable_v<ControlBlock>);
static_assert(std::is_standard_layout_v<SlotHeader> && std::is_trivially_copyable_v<SlotHeader>);

// Lock-free loads never write, which is what makes them legal on a PROT_READ mapping.
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::required_alignment == alignof(std::uint64_t));

/// Payload bytes per slot. Narrow contract: `g` is valid.
[[nodiscard]] constexpr std::uint32_t payload_capacity(RingGeometry g) noexcept {
  REVENANT_ASSERT(RingGeometry::is_valid(g.slot_size, g.slot_count));
  return g.slot_size - static_cast<std::uint32_t>(kSlotHeaderSize);
}

/// Exact file size of a segment. Narrow contract: `g` is valid.
[[nodiscard]] constexpr std::uint64_t segment_size(RingGeometry g) noexcept {
  return kSlotsOffset + g.slots_bytes();
}

/// seq_word while message `seq` is being written. Narrow contract: 1 <= seq <= kMaxSequence.
[[nodiscard]] constexpr std::uint64_t writing_word(std::uint64_t seq) noexcept {
  REVENANT_ASSERT(seq >= 1 && seq <= kMaxSequence);
  return 2 * seq - 1;
}

/// seq_word once message `seq` is committed. Narrow contract: 1 <= seq <= kMaxSequence.
[[nodiscard]] constexpr std::uint64_t committed_word(std::uint64_t seq) noexcept {
  REVENANT_ASSERT(seq >= 1 && seq <= kMaxSequence);
  return 2 * seq;
}

[[nodiscard]] constexpr bool is_committed(std::uint64_t word) noexcept {
  return word != 0 && word % 2 == 0;
}

/// Sequence a non-zero seq_word refers to, whether writing or committed.
[[nodiscard]] constexpr std::uint64_t sequence_of(std::uint64_t word) noexcept {
  return (word + 1) / 2;
}

[[nodiscard]] constexpr std::uint64_t pack_meta(std::uint32_t epoch,
                                                std::uint32_t length) noexcept {
  return (std::uint64_t{epoch} << 32) | length;
}

[[nodiscard]] constexpr std::uint32_t meta_epoch(std::uint64_t meta) noexcept {
  return static_cast<std::uint32_t>(meta >> 32);
}

[[nodiscard]] constexpr std::uint32_t meta_length(std::uint64_t meta) noexcept {
  return static_cast<std::uint32_t>(meta);
}

}  // namespace revenant::core

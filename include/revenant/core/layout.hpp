#pragma once

#include <revenant/config.hpp>
#include <revenant/core/ring.hpp>
#include <revenant/errors.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <system_error>
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

namespace detail {

inline constexpr std::uint64_t kFnvOffsetBasis = 0xcbf2'9ce4'8422'2325;
inline constexpr std::uint64_t kFnvPrime = 0x0000'0100'0000'01b3;

[[nodiscard]] constexpr std::uint64_t fnv1a_byte(std::uint64_t hash, std::uint8_t byte) noexcept {
  return (hash ^ std::uint64_t{byte}) * kFnvPrime;
}

[[nodiscard]] constexpr std::uint64_t fnv1a_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) {
    hash = fnv1a_byte(hash, static_cast<std::uint8_t>(value >> (8 * i)));
  }
  return hash;
}

}  // namespace detail

/// FNV-1a-64 over the wire constants, every struct size and field offset, and the geometry.
/// Two builds that disagree on any layout detail compute different hashes, so attaching fails
/// with layout_mismatch instead of misreading bytes.
[[nodiscard]] constexpr std::uint64_t layout_hash(RingGeometry g) noexcept {
  const std::array<std::uint64_t, 24> inputs{
      kWireVersion,
      kWireLine,
      kSlotsOffset,
      sizeof(SegmentHeader),
      offsetof(SegmentHeader, magic),
      offsetof(SegmentHeader, wire_version),
      offsetof(SegmentHeader, slot_size),
      offsetof(SegmentHeader, slot_count),
      offsetof(SegmentHeader, reserved0),
      offsetof(SegmentHeader, layout_hash),
      offsetof(SegmentHeader, created_unix_ns),
      offsetof(SegmentHeader, reserved),
      sizeof(ControlBlock),
      offsetof(ControlBlock, head),
      offsetof(ControlBlock, pad_head),
      offsetof(ControlBlock, epoch),
      offsetof(ControlBlock, pad_epoch),
      offsetof(ControlBlock, reserved),
      sizeof(SlotHeader),
      offsetof(SlotHeader, seq_word),
      offsetof(SlotHeader, meta),
      kSlotHeaderSize,
      g.slot_size,
      g.slot_count,
  };
  std::uint64_t hash = detail::kFnvOffsetBasis;
  for (const std::uint64_t value : inputs) {
    hash = detail::fnv1a_u64(hash, value);
  }
  return hash;
}

/// Typed views of a mapped segment. Narrow contract: `segment` is 8-byte aligned and at least
/// kSlotsOffset bytes long.
[[nodiscard]] inline SegmentHeader& header_of(std::byte* segment) noexcept {
  return *reinterpret_cast<SegmentHeader*>(segment + kHeaderOffset);
}
[[nodiscard]] inline const SegmentHeader& header_of(const std::byte* segment) noexcept {
  return *reinterpret_cast<const SegmentHeader*>(segment + kHeaderOffset);
}
[[nodiscard]] inline ControlBlock& control_of(std::byte* segment) noexcept {
  return *reinterpret_cast<ControlBlock*>(segment + kControlOffset);
}
[[nodiscard]] inline const ControlBlock& control_of(const std::byte* segment) noexcept {
  return *reinterpret_cast<const ControlBlock*>(segment + kControlOffset);
}

/// Writes a fresh segment: everything after `magic` is zeroed and the header filled in, then
/// `magic` is stored last with release ordering, publishing the rest to any acquiring reader.
/// `magic` itself is never written with a plain store, because a subscriber may be polling it.
/// Narrow contract: caller holds the publisher lease, `g` is valid, `segment.size()` equals
/// segment_size(g), `segment` is 8-byte aligned, and `magic` is 0.
inline void initialize_segment(std::span<std::byte> segment, RingGeometry g,
                               std::uint64_t created_unix_ns) noexcept {
  REVENANT_ASSERT(RingGeometry::is_valid(g.slot_size, g.slot_count));
  REVENANT_ASSERT(segment.size() == segment_size(g));
  REVENANT_ASSERT(reinterpret_cast<std::uintptr_t>(segment.data()) % alignof(std::uint64_t) == 0);

  std::atomic_ref<std::uint64_t> magic{header_of(segment.data()).magic};
  REVENANT_ASSERT(magic.load(std::memory_order_relaxed) == 0);

  constexpr std::size_t kAfterMagic = sizeof(SegmentHeader::magic);
  std::memset(segment.data() + kAfterMagic, 0, segment.size() - kAfterMagic);

  SegmentHeader header{};
  header.wire_version = kWireVersion;
  header.slot_size = g.slot_size;
  header.slot_count = g.slot_count;
  header.layout_hash = layout_hash(g);
  header.created_unix_ns = created_unix_ns;
  std::memcpy(segment.data() + kAfterMagic,
              reinterpret_cast<const std::byte*>(&header) + kAfterMagic,
              sizeof header - kAfterMagic);

  magic.store(kMagic, std::memory_order_release);
}

/// Outcome of validate_header: `geometry` is meaningful only when `error` is empty.
struct HeaderCheck {
  std::error_code error;
  RingGeometry geometry{};
};

/// Checks a mapped segment (its whole extent, so the size is the file size) in wire-format order:
/// incomplete, bad magic, version, impossible geometry or non-zero reserved bytes, size, layout.
/// Wide contract on contents: any bytes are handled safely. Narrow contract: 8-byte alignment.
[[nodiscard]] inline HeaderCheck validate_header(std::span<const std::byte> segment) noexcept {
  REVENANT_ASSERT(reinterpret_cast<std::uintptr_t>(segment.data()) % alignof(std::uint64_t) == 0);
  if (segment.size() < kSlotsOffset) {
    return {revenant::errc::segment_incomplete};
  }

  // A lock-free atomic load never writes, so the const_cast is safe even on a PROT_READ mapping.
  const std::uint64_t magic =
      std::atomic_ref<std::uint64_t>{const_cast<std::uint64_t&>(header_of(segment.data()).magic)}
          .load(std::memory_order_acquire);
  if (magic == 0) {
    return {revenant::errc::segment_incomplete};
  }
  if (magic != kMagic) {
    return {revenant::errc::bad_magic};
  }

  SegmentHeader header{};
  std::memcpy(&header, segment.data(), sizeof header);
  if (header.wire_version != kWireVersion) {
    return {revenant::errc::version_mismatch};
  }

  const RingGeometry geometry{header.slot_size, header.slot_count};
  const bool reserved_clear =
      header.reserved0 == 0 && header.reserved == decltype(header.reserved){};
  if (!RingGeometry::is_valid(geometry.slot_size, geometry.slot_count) || !reserved_clear) {
    return {revenant::errc::segment_corrupt};
  }
  if (segment.size() != segment_size(geometry)) {
    return {revenant::errc::segment_corrupt};
  }
  if (header.layout_hash != layout_hash(geometry)) {
    return {revenant::errc::layout_mismatch};
  }
  return {{}, geometry};
}

}  // namespace revenant::core

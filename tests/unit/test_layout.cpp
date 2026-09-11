#include <revenant/core/layout.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

namespace {

namespace core = revenant::core;
using core::ControlBlock;
using core::RingGeometry;
using core::SegmentHeader;
using core::SlotHeader;

template <typename T>
constexpr bool kIsWireStruct =
    std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T> && std::is_aggregate_v<T> &&
    std::has_unique_object_representations_v<T>;

static_assert(kIsWireStruct<SegmentHeader>);
static_assert(kIsWireStruct<ControlBlock>);
static_assert(kIsWireStruct<SlotHeader>);

TEST(Layout, SegmentMapMatchesTheWireFormat) {
  EXPECT_EQ(core::kHeaderOffset, 0U);
  EXPECT_EQ(core::kControlOffset, core::kHeaderOffset + sizeof(SegmentHeader));
  EXPECT_EQ(core::kSlotsOffset, core::kControlOffset + sizeof(ControlBlock));
  EXPECT_EQ(core::kSlotsOffset, 256U);
  EXPECT_EQ(core::kSlotsOffset % core::kWireLine, 0U);
}

TEST(Layout, SegmentHeaderFieldOffsets) {
  EXPECT_EQ(sizeof(SegmentHeader), 64U);
  EXPECT_EQ(alignof(SegmentHeader), 8U);
  EXPECT_EQ(offsetof(SegmentHeader, magic), 0U);
  EXPECT_EQ(offsetof(SegmentHeader, wire_version), 8U);
  EXPECT_EQ(offsetof(SegmentHeader, slot_size), 12U);
  EXPECT_EQ(offsetof(SegmentHeader, slot_count), 16U);
  EXPECT_EQ(offsetof(SegmentHeader, reserved0), 20U);
  EXPECT_EQ(offsetof(SegmentHeader, layout_hash), 24U);
  EXPECT_EQ(offsetof(SegmentHeader, created_unix_ns), 32U);
  EXPECT_EQ(offsetof(SegmentHeader, reserved), 40U);
}

TEST(Layout, ControlBlockKeepsEachWrittenFieldOnItsOwnLine) {
  EXPECT_EQ(sizeof(ControlBlock), 3 * core::kWireLine);
  EXPECT_EQ(offsetof(ControlBlock, head), 0U);
  EXPECT_EQ(offsetof(ControlBlock, epoch), core::kWireLine);
  EXPECT_EQ(offsetof(ControlBlock, reserved), 2 * core::kWireLine);
  EXPECT_EQ((core::kControlOffset + offsetof(ControlBlock, head)) % core::kWireLine, 0U);
  EXPECT_EQ((core::kControlOffset + offsetof(ControlBlock, epoch)) % core::kWireLine, 0U);
}

TEST(Layout, SlotHeaderIsTwoWordsFollowedByAnAlignedPayload) {
  EXPECT_EQ(sizeof(SlotHeader), core::kSlotHeaderSize);
  EXPECT_EQ(offsetof(SlotHeader, seq_word), 0U);
  EXPECT_EQ(offsetof(SlotHeader, meta), 8U);
  EXPECT_EQ(core::kSlotHeaderSize % sizeof(std::uint64_t), 0U);
}

TEST(Layout, SharedWordsAreLockFreeSoReadOnlyMappingsCanLoadThem) {
  EXPECT_TRUE(std::atomic_ref<std::uint64_t>::is_always_lock_free);
  EXPECT_TRUE(std::atomic_ref<std::uint32_t>::is_always_lock_free);
  EXPECT_EQ(std::atomic_ref<std::uint64_t>::required_alignment, alignof(std::uint64_t));
}

TEST(Layout, MagicSpellsRevnant1) {
  std::string spelled;
  for (int shift = 56; shift >= 0; shift -= 8) {
    spelled.push_back(static_cast<char>((core::kMagic >> shift) & 0xFF));
  }
  EXPECT_EQ(spelled, "REVNANT1");
  EXPECT_EQ(core::kWireVersion, 1U);
}

TEST(Layout, SegmentSizeAndPayloadCapacityAtTheGeometryBounds) {
  constexpr RingGeometry smallest{core::kMinSlotSize, core::kMinSlotCount};
  EXPECT_EQ(core::segment_size(smallest), 256U + 2 * 64);
  EXPECT_EQ(core::payload_capacity(smallest), 48U);

  constexpr RingGeometry largest{core::kMaxSlotSize, core::kMaxSlotCount};
  EXPECT_EQ(core::segment_size(largest), 256 + (std::uint64_t{1} << 36));
  EXPECT_EQ(core::payload_capacity(largest), 4080U);
}

TEST(Layout, SeqWordEncodesWritingAsOddAndCommittedAsEven) {
  for (const std::uint64_t s :
       {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{1'000'003}, core::kMaxSequence}) {
    const std::uint64_t writing = core::writing_word(s);
    const std::uint64_t committed = core::committed_word(s);
    EXPECT_EQ(writing % 2, 1U) << s;
    EXPECT_EQ(committed % 2, 0U) << s;
    EXPECT_EQ(committed, writing + 1) << s;
    EXPECT_EQ(core::sequence_of(writing), s);
    EXPECT_EQ(core::sequence_of(committed), s);
    EXPECT_FALSE(core::is_committed(writing));
    EXPECT_TRUE(core::is_committed(committed));
  }
  EXPECT_FALSE(core::is_committed(0));
  EXPECT_EQ(core::committed_word(core::kMaxSequence),
            std::numeric_limits<std::uint64_t>::max() - 1);
}

// A slot's word only ever increases: 0 < 2s-1 < 2s < 2(s+N)-1, which is what lets a reader
// compare words to tell "not yet written" from "overwritten by a later lap".
TEST(Layout, SeqWordsIncreaseAcrossWritesAndLaps) {
  constexpr std::uint64_t kLap = 8;
  for (std::uint64_t s = 1; s < 100; ++s) {
    EXPECT_LT(core::writing_word(s), core::committed_word(s));
    EXPECT_LT(core::committed_word(s), core::writing_word(s + kLap));
  }
}

TEST(Layout, MetaPacksEpochAndLength) {
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 4> cases{{
      {0, 0},
      {1, 48},
      {7, 4080},
      {std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()},
  }};
  for (const auto& [epoch, length] : cases) {
    const std::uint64_t meta = core::pack_meta(epoch, length);
    EXPECT_EQ(core::meta_epoch(meta), epoch);
    EXPECT_EQ(core::meta_length(meta), length);
  }
  EXPECT_EQ(core::pack_meta(2, 3), (std::uint64_t{2} << 32) | 3);
}

#ifndef NDEBUG
TEST(LayoutDeathTest, SequenceZeroHasNoWord) {
  EXPECT_DEATH(static_cast<void>(core::writing_word(0)), "assertion failed");
}

TEST(LayoutDeathTest, SequencesStopBelowTwoToTheSixtyThree) {
  EXPECT_DEATH(static_cast<void>(core::committed_word(core::kMaxSequence + 1)), "assertion failed");
}
#endif

}  // namespace

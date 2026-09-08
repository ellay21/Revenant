#include <revenant/core/ring.hpp>

#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <limits>
#include <random>
#include <set>

namespace {

using revenant::core::kMaxSlotCount;
using revenant::core::kMaxSlotSize;
using revenant::core::kMinSlotCount;
using revenant::core::kMinSlotSize;
using revenant::core::RingGeometry;

constexpr std::uint64_t kTopOfSequenceSpace = std::uint64_t{1} << 63;

// The whole component is usable in constant expressions.
static_assert(RingGeometry::is_valid(64, 2));
static_assert(RingGeometry::is_valid(kMaxSlotSize, kMaxSlotCount));
static_assert(!RingGeometry::is_valid(96, 8));
static_assert(RingGeometry{256, 8}.index_of(9) == 1);
static_assert(RingGeometry{256, 8}.slot_offset(9) == 256);
static_assert(RingGeometry{kMaxSlotSize, kMaxSlotCount}.slots_bytes() == std::uint64_t{1} << 36);
static_assert(RingGeometry{256, 8} == RingGeometry{256, 8});
static_assert(RingGeometry{256, 8} != RingGeometry{256, 16});

TEST(RingGeometry, AcceptsEveryPowerOfTwoWithinBounds) {
  for (std::uint32_t size = kMinSlotSize; size <= kMaxSlotSize; size *= 2) {
    for (std::uint32_t count = kMinSlotCount; count <= kMaxSlotCount; count *= 2) {
      EXPECT_TRUE(RingGeometry::is_valid(size, count)) << size << " x " << count;
    }
  }
}

TEST(RingGeometry, RejectsSlotSizesOutOfRangeOrNotPowersOfTwo) {
  for (const std::uint32_t size : {0U, 1U, 3U, 32U, 63U, 65U, 96U, 4095U, 8192U, 0xFFFF'FFFFU}) {
    EXPECT_FALSE(RingGeometry::is_valid(size, 8)) << "slot_size " << size;
  }
}

TEST(RingGeometry, RejectsSlotCountsOutOfRangeOrNotPowersOfTwo) {
  for (const std::uint32_t count : {0U, 1U, 3U, 6U, 1000U, kMaxSlotCount + 1, kMaxSlotCount * 2}) {
    EXPECT_FALSE(RingGeometry::is_valid(256, count)) << "slot_count " << count;
  }
}

TEST(RingGeometry, ConsecutiveSequencesVisitEverySlotExactlyOncePerLap) {
  constexpr RingGeometry g{128, 16};
  for (std::uint64_t lap_start : {std::uint64_t{1}, std::uint64_t{17}, kTopOfSequenceSpace - 16}) {
    std::set<std::uint32_t> seen;
    for (std::uint64_t s = lap_start; s < lap_start + g.slot_count; ++s) {
      seen.insert(g.index_of(s));
    }
    EXPECT_EQ(seen.size(), g.slot_count) << "lap starting at " << lap_start;
  }
}

TEST(RingGeometry, SequenceOneLapLaterMapsToTheSameSlot) {
  for (const RingGeometry g :
       {RingGeometry{64, 2}, RingGeometry{256, 4096}, RingGeometry{kMaxSlotSize, kMaxSlotCount}}) {
    for (const std::uint64_t s : {std::uint64_t{1}, std::uint64_t{12345}, kTopOfSequenceSpace - 1,
                                  kTopOfSequenceSpace, kTopOfSequenceSpace + g.slot_count}) {
      EXPECT_EQ(g.index_of(s), g.index_of(s + g.slot_count)) << s;
    }
  }
}

TEST(RingGeometry, IndexIsCorrectAtTheTopOfTheSequenceSpace) {
  constexpr RingGeometry g{256, 1024};
  constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(g.index_of(max), g.slot_count - 1);
  EXPECT_EQ(g.index_of(kTopOfSequenceSpace), 0U);
  EXPECT_EQ(g.slot_offset(max), std::uint64_t{g.slot_count - 1} * g.slot_size);
}

TEST(RingGeometry, SlotsBytesAtMaximumGeometryDoesNotOverflow) {
  constexpr RingGeometry g{kMaxSlotSize, kMaxSlotCount};
  EXPECT_EQ(g.slots_bytes(), std::uint64_t{kMaxSlotSize} * kMaxSlotCount);
  EXPECT_EQ(g.slot_offset(g.slot_count), 0U);
  EXPECT_EQ(g.slot_offset(g.slot_count - 1), g.slots_bytes() - g.slot_size);
}

// Seeded so any failure reproduces exactly.
TEST(RingGeometry, MatchesModuloArithmeticForRandomGeometriesAndSequences) {
  std::mt19937_64 rng{0x5245'564E'414E'5431};
  std::uniform_int_distribution<int> size_log2{6, 12};
  std::uniform_int_distribution<int> count_log2{1, 24};
  std::uniform_int_distribution<std::uint64_t> seq{1, std::numeric_limits<std::uint64_t>::max()};

  for (int i = 0; i < 100'000; ++i) {
    const RingGeometry g{std::uint32_t{1} << size_log2(rng), std::uint32_t{1} << count_log2(rng)};
    ASSERT_TRUE(RingGeometry::is_valid(g.slot_size, g.slot_count));
    const std::uint64_t s = seq(rng);

    const std::uint32_t index = g.index_of(s);
    ASSERT_EQ(index, s % g.slot_count) << g.slot_size << " x " << g.slot_count << " seq " << s;
    ASSERT_LT(index, g.slot_count);

    const std::uint64_t offset = g.slot_offset(s);
    ASSERT_EQ(offset % g.slot_size, 0U);
    ASSERT_LE(offset + g.slot_size, g.slots_bytes());
  }
}

TEST(RingGeometry, MaskSelectsTheLowBits) {
  for (std::uint32_t count = kMinSlotCount; count <= kMaxSlotCount; count *= 2) {
    const RingGeometry g{64, count};
    EXPECT_EQ(g.mask(), count - 1);
    EXPECT_EQ(std::popcount(g.mask()), std::countr_zero(count));
  }
}

#ifndef NDEBUG
TEST(RingGeometryDeathTest, IndexOfInvalidGeometryViolatesTheContract) {
  const RingGeometry invalid{256, 0};
  EXPECT_DEATH(static_cast<void>(invalid.index_of(1)), "assertion failed");
}
#endif

}  // namespace

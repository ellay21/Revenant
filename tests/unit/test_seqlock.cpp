#include <revenant/core/layout.hpp>
#include <revenant/core/seqlock.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace {

namespace core = revenant::core;
using core::SlotRead;
using core::SlotState;

constexpr std::uint32_t kSlotSize = 256;
constexpr std::uint32_t kCapacity = kSlotSize - core::kSlotHeaderSize;
constexpr std::uint64_t kLap = 8;  // slot_count of the imaginary ring this slot belongs to
constexpr std::byte kSentinel{0xEE};

std::vector<std::byte> pattern(std::size_t size, std::uint8_t seed) {
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::byte>(seed + 31 * i);
  }
  return bytes;
}

class Seqlock : public ::testing::Test {
 protected:
  // Exactly one slot on the heap, so AddressSanitizer flags any access outside it.
  std::unique_ptr<std::byte[]> storage_ = std::make_unique<std::byte[]>(kSlotSize);
  std::byte* slot_ = storage_.get();
  std::vector<std::byte> out_ = std::vector<std::byte>(kCapacity, kSentinel);

  SlotRead read(std::uint64_t expected) {
    return core::read_slot(slot_, kCapacity, expected, out_);
  }

  void write(std::uint64_t seq, std::uint32_t epoch, std::span<const std::byte> payload) {
    core::write_slot(slot_, kCapacity, seq, epoch, payload);
  }

  void set_seq_word(std::uint64_t word) { std::memcpy(slot_, &word, sizeof word); }

  void set_meta(std::uint64_t meta) { std::memcpy(slot_ + 8, &meta, sizeof meta); }
};

TEST_F(Seqlock, NeverWrittenSlotIsNotYet) {
  const SlotRead r = read(1);
  EXPECT_EQ(r.state, SlotState::kNotYet);
  EXPECT_EQ(r.observed_seq, 0U);
}

TEST_F(Seqlock, SlotHoldingAnOlderMessageIsNotYet) {
  write(3, 1, pattern(10, 1));
  const SlotRead r = read(3 + kLap);
  EXPECT_EQ(r.state, SlotState::kNotYet);
  EXPECT_EQ(r.observed_seq, 3U);
}

TEST_F(Seqlock, OddWordForTheExpectedSequenceIsInProgress) {
  set_seq_word(core::writing_word(5));
  const SlotRead r = read(5);
  EXPECT_EQ(r.state, SlotState::kInProgress);
  EXPECT_EQ(r.observed_seq, 5U);
}

TEST_F(Seqlock, CommittedMessageRoundTripsExactlyForEveryLengthShape) {
  for (const std::size_t size :
       {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{8}, std::size_t{9},
        std::size_t{15}, std::size_t{16}, std::size_t{kCapacity - 1}, std::size_t{kCapacity}}) {
    std::fill(out_.begin(), out_.end(), kSentinel);
    const std::vector<std::byte> payload = pattern(size, static_cast<std::uint8_t>(size));
    const std::uint64_t seq = 100 + size;
    write(seq, 7, payload);

    const SlotRead r = read(seq);
    ASSERT_EQ(r.state, SlotState::kCommitted) << size;
    EXPECT_EQ(r.observed_seq, seq);
    EXPECT_EQ(r.epoch, 7U);
    EXPECT_EQ(r.length, size);
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), out_.begin())) << size;
    EXPECT_TRUE(std::all_of(out_.begin() + static_cast<std::ptrdiff_t>(size), out_.end(),
                            [](std::byte b) { return b == kSentinel; }))
        << "reader wrote past the message length " << size;
  }
}

TEST_F(Seqlock, SlotAlreadyHoldingANewerMessageIsLapped) {
  write(5 + kLap, 1, pattern(4, 2));
  const SlotRead committed = read(5);
  EXPECT_EQ(committed.state, SlotState::kLapped);
  EXPECT_EQ(committed.observed_seq, 5 + kLap);

  set_seq_word(core::writing_word(5 + 2 * kLap));
  const SlotRead writing = read(5);
  EXPECT_EQ(writing.state, SlotState::kLapped);
  EXPECT_EQ(writing.observed_seq, 5 + 2 * kLap);
}

TEST_F(Seqlock, INV4_OverwriteDuringTheCopyIsTorn) {
  write(5, 1, pattern(kCapacity, 3));
  const std::vector<std::byte> newer = pattern(kCapacity, 4);
  int hook_calls = 0;

  const SlotRead r = core::read_slot(slot_, kCapacity, 5, out_, [&] {
    ++hook_calls;
    write(5 + kLap, 1, newer);
  });

  EXPECT_EQ(hook_calls, 1);
  EXPECT_EQ(r.state, SlotState::kTorn);
  EXPECT_EQ(r.observed_seq, 5 + kLap);
}

TEST_F(Seqlock, INV4_WriterMerelyStartingDuringTheCopyIsTorn) {
  write(5, 1, pattern(32, 3));
  const SlotRead r = core::read_slot(slot_, kCapacity, 5, out_,
                                     [&] { set_seq_word(core::writing_word(5 + kLap)); });
  EXPECT_EQ(r.state, SlotState::kTorn);
  EXPECT_EQ(r.observed_seq, 5 + kLap);
}

TEST_F(Seqlock, HookThatChangesNothingStillYieldsTheCommittedMessage) {
  write(5, 2, pattern(20, 5));
  int hook_calls = 0;
  const SlotRead r = core::read_slot(slot_, kCapacity, 5, out_, [&] { ++hook_calls; });
  EXPECT_EQ(hook_calls, 1);
  EXPECT_EQ(r.state, SlotState::kCommitted);
}

TEST_F(Seqlock, INV7_ForgedLengthIsCorruptAndNothingIsCopied) {
  for (const std::uint32_t length : {kCapacity + 1, std::numeric_limits<std::uint32_t>::max()}) {
    std::fill(out_.begin(), out_.end(), kSentinel);
    write(5, 1, pattern(16, 6));
    set_meta(core::pack_meta(1, length));

    const SlotRead r = read(5);
    EXPECT_EQ(r.state, SlotState::kCorrupt) << length;
    EXPECT_EQ(r.length, length);
    EXPECT_TRUE(std::all_of(out_.begin(), out_.end(), [](std::byte b) { return b == kSentinel; }));
  }
}

TEST_F(Seqlock, INV2_SeqWordOnlyIncreasesAcrossLaps) {
  std::uint64_t previous = core::load_seq_word(slot_);
  EXPECT_EQ(previous, 0U);
  for (std::uint64_t seq = 3; seq < 3 + 10 * kLap; seq += kLap) {
    write(seq, 1, pattern(8, 7));
    const std::uint64_t word = core::load_seq_word(slot_);
    EXPECT_EQ(word, core::committed_word(seq));
    EXPECT_GT(word, previous);
    previous = word;
  }
}

TEST_F(Seqlock, ShorterMessageAfterALongerOneReportsOnlyItsOwnLength) {
  write(5, 1, pattern(kCapacity, 8));
  const std::vector<std::byte> shorter = pattern(3, 9);
  write(5 + kLap, 1, shorter);

  const SlotRead r = read(5 + kLap);
  ASSERT_EQ(r.state, SlotState::kCommitted);
  EXPECT_EQ(r.length, 3U);
  EXPECT_TRUE(std::equal(shorter.begin(), shorter.end(), out_.begin()));
}

#ifndef NDEBUG
TEST_F(Seqlock, PayloadLargerThanCapacityViolatesTheContract) {
  const std::vector<std::byte> too_big(kCapacity + 1);
  EXPECT_DEATH(write(1, 1, too_big), "assertion failed");
}

TEST_F(Seqlock, OutputSmallerThanCapacityViolatesTheContract) {
  std::vector<std::byte> small(kCapacity - 1);
  EXPECT_DEATH(static_cast<void>(core::read_slot(slot_, kCapacity, 1, small)), "assertion failed");
}
#endif

TEST(SlotAt, AddressesTheSlotOfASequence) {
  constexpr core::RingGeometry g{128, 8};
  std::vector<std::byte> segment(core::segment_size(g));
  std::byte* base = segment.data();
  EXPECT_EQ(core::slot_at(base, g, 1), base + core::kSlotsOffset + 128);
  EXPECT_EQ(core::slot_at(base, g, 8), base + core::kSlotsOffset);
  EXPECT_EQ(core::slot_at(base, g, 9), core::slot_at(base, g, 1));
  const std::byte* const_base = base;
  EXPECT_EQ(core::slot_at(const_base, g, 7), base + core::kSlotsOffset + 7 * std::size_t{128});
}

}  // namespace

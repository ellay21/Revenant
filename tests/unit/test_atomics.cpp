#include <revenant/core/atomics.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

namespace atomics = revenant::core::atomics;

static_assert(atomics::SharedWord<std::uint32_t>);
static_assert(atomics::SharedWord<std::uint64_t>);
static_assert(!atomics::SharedWord<std::uint16_t>);
static_assert(!atomics::SharedWord<std::int64_t>);

constexpr std::byte kSentinel{0xEE};
constexpr std::size_t kMaxBytes = 64;

std::vector<std::byte> pattern(std::size_t size) {
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    bytes[i] = static_cast<std::byte>(0x40 + i);
  }
  return bytes;
}

struct alignas(8) SharedBuffer {
  std::array<std::byte, kMaxBytes + 8> bytes;
};

TEST(Atomics, NamedLoadsObserveNamedStores) {
  std::uint64_t word64 = 0;
  std::uint32_t word32 = 0;
  atomics::store_relaxed(word64, 7);
  EXPECT_EQ(atomics::load_relaxed(word64), 7U);
  atomics::store_release(word64, 9);
  atomics::store_release(word32, 11);
  const std::uint64_t& read_only64 = word64;
  const std::uint32_t& read_only32 = word32;
  EXPECT_EQ(atomics::load_acquire(read_only64), 9U);
  EXPECT_EQ(atomics::load_acquire(read_only32), 11U);
}

TEST(Atomics, WordCopiesRoundTripEveryLength) {
  for (std::size_t n = 0; n <= kMaxBytes; ++n) {
    SharedBuffer shared{};
    std::fill(shared.bytes.begin(), shared.bytes.end(), kSentinel);
    const std::vector<std::byte> source = pattern(n);
    atomics::store_words_relaxed(shared.bytes.data(), source);

    std::vector<std::byte> out(n);
    atomics::load_words_relaxed(shared.bytes.data(), out);
    EXPECT_EQ(out, source) << n;
  }
}

TEST(Atomics, StoreZeroPadsTheLastWordAndTouchesNothingAfterIt) {
  for (std::size_t n = 0; n <= kMaxBytes; ++n) {
    SharedBuffer shared{};
    std::fill(shared.bytes.begin(), shared.bytes.end(), kSentinel);
    atomics::store_words_relaxed(shared.bytes.data(), pattern(n));

    const std::size_t rounded = (n + 7) / 8 * 8;
    const auto begin = shared.bytes.begin();
    EXPECT_TRUE(std::all_of(begin + static_cast<std::ptrdiff_t>(n),
                            begin + static_cast<std::ptrdiff_t>(rounded),
                            [](std::byte b) { return b == std::byte{0}; }))
        << n;
    EXPECT_TRUE(std::all_of(begin + static_cast<std::ptrdiff_t>(rounded), shared.bytes.end(),
                            [](std::byte b) { return b == kSentinel; }))
        << n;
  }
}

TEST(Atomics, LoadNeverWritesPastTheRequestedLength) {
  SharedBuffer shared{};
  atomics::store_words_relaxed(shared.bytes.data(), pattern(kMaxBytes));
  for (std::size_t n = 0; n <= kMaxBytes - 8; ++n) {
    std::vector<std::byte> out(n + 8, kSentinel);
    atomics::load_words_relaxed(shared.bytes.data(), std::span{out}.first(n));
    EXPECT_TRUE(std::all_of(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(),
                            [](std::byte b) { return b == kSentinel; }))
        << n;
  }
}

#ifndef NDEBUG
TEST(AtomicsDeathTest, MisalignedWordsViolateTheContract) {
  SharedBuffer shared{};
  const std::vector<std::byte> source = pattern(8);
  EXPECT_DEATH(atomics::store_words_relaxed(shared.bytes.data() + 1, source), "assertion failed");
}
#endif

}  // namespace

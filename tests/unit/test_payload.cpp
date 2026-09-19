#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>

#include "support/payload.hpp"

namespace {

namespace testing = revenant::testing;

constexpr std::size_t kCapacity = 240;

class Payload : public ::testing::Test {
 protected:
  std::array<std::byte, kCapacity> buffer_{};

  std::span<std::byte> make(std::uint64_t seq, std::uint32_t epoch) {
    return std::span{buffer_}.first(testing::make_payload(seq, epoch, kCapacity, buffer_));
  }
};

TEST_F(Payload, RoundTripsAndStaysWithinBounds) {
  std::set<std::size_t> lengths;
  for (std::uint64_t seq = 1; seq <= 10'000; ++seq) {
    const auto bytes = make(seq, 3);
    ASSERT_GE(bytes.size(), testing::kMinPayload);
    ASSERT_LE(bytes.size(), kCapacity);
    ASSERT_TRUE(testing::check_payload(seq, 3, kCapacity, bytes)) << seq;
    lengths.insert(bytes.size());
  }
  EXPECT_GT(lengths.size(), kCapacity / 2) << "lengths should vary across messages";
}

TEST_F(Payload, IsDeterministic) {
  const auto first = make(42, 7);
  std::array<std::byte, kCapacity> again{};
  const std::size_t length = testing::make_payload(42, 7, kCapacity, again);
  ASSERT_EQ(length, first.size());
  EXPECT_TRUE(std::equal(first.begin(), first.end(), again.begin()));
}

TEST_F(Payload, AnySingleFlippedBitIsDetected) {
  const auto bytes = make(1234, 5);
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      bytes[i] ^= static_cast<std::byte>(1 << bit);
      EXPECT_FALSE(testing::check_payload(1234, 5, kCapacity, bytes)) << i << ":" << bit;
      bytes[i] ^= static_cast<std::byte>(1 << bit);
    }
  }
  EXPECT_TRUE(testing::check_payload(1234, 5, kCapacity, bytes));
}

TEST_F(Payload, WrongLengthIsDetected) {
  const auto bytes = make(99, 1);
  EXPECT_FALSE(testing::check_payload(99, 1, kCapacity, bytes.first(bytes.size() - 1)));
  if (bytes.size() < kCapacity) {
    EXPECT_FALSE(
        testing::check_payload(99, 1, kCapacity, std::span{buffer_}.first(bytes.size() + 1)));
  }
}

TEST_F(Payload, AnotherMessagesPayloadIsDetected) {
  for (std::uint64_t seq = 1; seq <= 1'000; ++seq) {
    const auto bytes = make(seq, 2);
    EXPECT_FALSE(testing::check_payload(seq + 1, 2, kCapacity, bytes)) << seq;
    EXPECT_FALSE(testing::check_payload(seq + 8, 2, kCapacity, bytes)) << seq;
    EXPECT_FALSE(testing::check_payload(seq, 3, kCapacity, bytes)) << seq;
  }
}

}  // namespace

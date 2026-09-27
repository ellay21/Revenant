#include <revenant/channel/publisher.hpp>
#include <revenant/core/layout.hpp>
#include <revenant/core/seqlock.hpp>
#include <revenant/errors.hpp>
#include <revenant/platform/lease.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

#include "support/payload.hpp"
#include "support/unique_channel.hpp"

namespace {

namespace core = revenant::core;
namespace platform = revenant::platform;
using namespace std::chrono_literals;
using revenant::ChannelConfig;
using revenant::errc;
using revenant::LeaseMode;
using revenant::Publisher;
using revenant::testing::UniqueChannel;

constexpr ChannelConfig kConfig{.slot_size = 128, .slot_count = 8};
constexpr core::RingGeometry kGeometry = kConfig.geometry();
constexpr std::uint32_t kCapacity = kConfig.payload_capacity();

template <typename F>
std::error_code error_of(F&& f) {
  try {
    std::forward<F>(f)();
  } catch (const std::system_error& e) {
    return e.code();
  }
  return {};
}

// A second, independent read-write mapping of the channel: what a test uses to inspect the
// segment or to forge the state a crashed publisher would leave behind.
class Segment {
 public:
  explicit Segment(const std::string& name)
      : segment_(platform::ShmSegment::open(name, platform::Access::kReadWrite)) {
    segment_.map(platform::Access::kReadWrite);
  }
  [[nodiscard]] std::byte* base() const { return segment_.bytes().data(); }
  [[nodiscard]] std::uint64_t head() const { return core::control_of(base()).head; }
  [[nodiscard]] std::uint32_t epoch() const { return core::control_of(base()).epoch; }
  [[nodiscard]] std::byte* slot(std::uint64_t seq) const {
    return core::slot_at(base(), kGeometry, seq);
  }

 private:
  platform::ShmSegment segment_;
};

std::uint64_t publish_payload(Publisher& publisher, std::uint64_t expected_seq) {
  std::array<std::byte, kCapacity> payload{};
  const std::size_t length =
      revenant::testing::make_payload(expected_seq, publisher.epoch(), kCapacity, payload);
  return publisher.publish(std::span{payload}.first(length));
}

bool slot_holds(const Segment& segment, std::uint64_t seq, std::uint32_t epoch) {
  std::array<std::byte, kCapacity> out{};
  const core::SlotRead r = core::read_slot(segment.slot(seq), kCapacity, seq, out);
  return r.state == core::SlotState::kCommitted && r.epoch == epoch &&
         revenant::testing::check_payload(seq, epoch, kCapacity, std::span{out}.first(r.length));
}

TEST(Publisher, FreshChannelStartsAtEpochOneWithNothingPublished) {
  const UniqueChannel channel;
  const Publisher publisher = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(publisher.epoch(), 1U);
  EXPECT_EQ(publisher.last_sequence(), 0U);
  EXPECT_EQ(publisher.payload_capacity(), kCapacity);

  const Segment segment{channel.name()};
  EXPECT_EQ(segment.head(), 0U);
  EXPECT_EQ(segment.epoch(), 1U);
}

TEST(Publisher, PublishAssignsConsecutiveSequencesAndAdvancesHead) {
  const UniqueChannel channel;
  Publisher publisher = Publisher::create(channel.name(), kConfig);
  const Segment segment{channel.name()};

  for (std::uint64_t seq = 1; seq <= 20; ++seq) {
    ASSERT_EQ(publish_payload(publisher, seq), seq);
    EXPECT_EQ(segment.head(), seq);
    EXPECT_TRUE(slot_holds(segment, seq, 1)) << seq;
  }
  EXPECT_EQ(publisher.last_sequence(), 20U);
}

TEST(Publisher, EmptyAndFullCapacityPayloadsArePublished) {
  const UniqueChannel channel;
  Publisher publisher = Publisher::create(channel.name(), kConfig);
  const std::array<std::byte, kCapacity> full{};
  EXPECT_EQ(publisher.publish({}), 1U);
  EXPECT_EQ(publisher.publish(full), 2U);
}

TEST(Publisher, RestartResumesTheSequenceUnderTheNextEpoch) {
  const UniqueChannel channel;
  {
    Publisher first = Publisher::create(channel.name(), kConfig);
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
      publish_payload(first, seq);
    }
  }
  Publisher second = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(second.epoch(), 2U);
  EXPECT_EQ(second.last_sequence(), 5U);
  EXPECT_EQ(publish_payload(second, 6), 6U);
  EXPECT_TRUE(slot_holds(Segment{channel.name()}, 6, 2));
}

TEST(Publisher, INV1_SecondPublisherIsRejectedWhileTheFirstIsLive) {
  const UniqueChannel channel;
  std::optional<Publisher> first = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(error_of([&] { static_cast<void>(Publisher::create(channel.name(), kConfig)); }),
            errc::publisher_active);
  first.reset();
  EXPECT_FALSE(error_of([&] { static_cast<void>(Publisher::create(channel.name(), kConfig)); }));
}

TEST(Publisher, MovingTransfersTheLease) {
  const UniqueChannel channel;
  Publisher original = Publisher::create(channel.name(), kConfig);
  std::optional<Publisher> moved{std::move(original)};
  EXPECT_EQ(error_of([&] { static_cast<void>(Publisher::create(channel.name(), kConfig)); }),
            errc::publisher_active);
  EXPECT_EQ(publish_payload(*moved, 1), 1U);
  moved.reset();
  EXPECT_FALSE(error_of([&] { static_cast<void>(Publisher::create(channel.name(), kConfig)); }));
}

TEST(Publisher, ExistingChannelWithAnotherGeometryIsRejected) {
  const UniqueChannel channel;
  static_cast<void>(Publisher::create(channel.name(), kConfig));
  const ChannelConfig other{.slot_size = 256, .slot_count = 8};
  EXPECT_EQ(error_of([&] { static_cast<void>(Publisher::create(channel.name(), other)); }),
            errc::geometry_mismatch);
}

TEST(Publisher, InvalidConfigAndNamesAreRejectedBeforeTouchingDevShm) {
  const UniqueChannel channel;
  EXPECT_EQ(
      error_of([&] { static_cast<void>(Publisher::create(channel.name(), {.slot_size = 96})); }),
      errc::invalid_config);
  EXPECT_EQ(error_of([&] { static_cast<void>(Publisher::create("../etc", kConfig)); }),
            errc::invalid_name);
  EXPECT_EQ(
      error_of([&] {
        static_cast<void>(platform::ShmSegment::open(channel.name(), platform::Access::kReadOnly));
      }),
      errc::channel_not_found);
}

// INV3: the previous publisher died between W4 (commit) and W5 (head).
TEST(Publisher, INV3_RecoveryRollsHeadForwardOverACommittedSlot) {
  const UniqueChannel channel;
  {
    Publisher first = Publisher::create(channel.name(), kConfig);
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
      publish_payload(first, seq);
    }
    std::array<std::byte, kCapacity> payload{};
    const std::size_t length = revenant::testing::make_payload(4, 1, kCapacity, payload);
    const Segment segment{channel.name()};
    core::write_slot(segment.slot(4), kCapacity, 4, 1, std::span{payload}.first(length));
  }
  Publisher second = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(second.last_sequence(), 4U);
  EXPECT_EQ(Segment{channel.name()}.head(), 4U);
  EXPECT_EQ(publish_payload(second, 5), 5U);
}

// The previous publisher died inside W1-W3: message 4 was never committed, so nobody saw it,
// and reusing sequence 4 creates no duplicate.
TEST(Publisher, InProgressSlotIsReusedByTheNextPublisher) {
  const UniqueChannel channel;
  {
    Publisher first = Publisher::create(channel.name(), kConfig);
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
      publish_payload(first, seq);
    }
    const std::uint64_t writing = core::writing_word(4);
    std::memcpy(Segment{channel.name()}.slot(4), &writing, sizeof writing);
  }
  Publisher second = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(second.last_sequence(), 3U);
  EXPECT_EQ(publish_payload(second, 4), 4U);
  EXPECT_TRUE(slot_holds(Segment{channel.name()}, 4, 2));
}

// F7: the creator died after sizing the file but before publishing magic.
TEST(Publisher, F7_SegmentWhoseCreatorDiedMidInitialisationIsReinitialised) {
  const UniqueChannel channel;
  {
    platform::ShmSegment leftover = platform::ShmSegment::open_or_create(channel.name());
    leftover.reserve(core::segment_size(kGeometry));
    leftover.map(platform::Access::kReadWrite);
    std::memset(leftover.bytes().data() + 8, 0xAB, leftover.bytes().size() - 8);
  }
  Publisher publisher = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(publisher.epoch(), 1U);
  EXPECT_EQ(publisher.last_sequence(), 0U);
  EXPECT_EQ(publish_payload(publisher, 1), 1U);
  EXPECT_TRUE(slot_holds(Segment{channel.name()}, 1, 1));
}

TEST(Publisher, TruncatedLeftoverWithGarbageIsReinitialised) {
  const UniqueChannel channel;
  {
    platform::ShmSegment leftover = platform::ShmSegment::open_or_create(channel.name());
    leftover.reserve(100);
    leftover.map(platform::Access::kReadWrite);
    std::memset(leftover.bytes().data(), 0xCD, leftover.bytes().size());
  }
  Publisher publisher = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(publisher.epoch(), 1U);
  EXPECT_EQ(publish_payload(publisher, 1), 1U);
  EXPECT_TRUE(slot_holds(Segment{channel.name()}, 1, 1));
}

TEST(Publisher, ForeignFileIsNeverOverwritten) {
  const UniqueChannel channel;
  {
    platform::ShmSegment foreign = platform::ShmSegment::open_or_create(channel.name());
    foreign.reserve(4096);
    foreign.map(platform::Access::kReadWrite);
    const std::uint64_t elf = 0x464C457F;
    std::memcpy(foreign.bytes().data(), &elf, sizeof elf);
  }
  EXPECT_EQ(error_of([&] { static_cast<void>(Publisher::create(channel.name(), kConfig)); }),
            errc::bad_magic);
}

TEST(Publisher, ImpossibleHeadIsCorruptRatherThanOverflowing) {
  const UniqueChannel channel;
  static_cast<void>(Publisher::create(channel.name(), kConfig));
  core::control_of(Segment{channel.name()}.base()).head = core::kMaxSequence;
  EXPECT_EQ(error_of([&] { static_cast<void>(Publisher::create(channel.name(), kConfig)); }),
            errc::segment_corrupt);
}

TEST(Publisher, HotStandbyTakesOverAsSoonAsTheActivePublisherGoesAway) {
  const UniqueChannel channel;
  std::optional<Publisher> active = Publisher::create(channel.name(), kConfig);
  for (std::uint64_t seq = 1; seq <= 3; ++seq) {
    publish_payload(*active, seq);
  }

  std::promise<std::pair<std::uint32_t, std::uint64_t>> took_over;
  std::future<std::pair<std::uint32_t, std::uint64_t>> result = took_over.get_future();
  std::thread standby{[&] {
    const Publisher p = Publisher::create(channel.name(), kConfig, LeaseMode::kWaitForLease);
    took_over.set_value({p.epoch(), p.last_sequence()});
  }};

  EXPECT_EQ(result.wait_for(50ms), std::future_status::timeout) << "standby must wait";
  active.reset();
  ASSERT_EQ(result.wait_for(10s), std::future_status::ready);
  standby.join();
  const auto [epoch, last] = result.get();
  EXPECT_EQ(epoch, 2U);
  EXPECT_EQ(last, 3U);
}

TEST(Publisher, ChannelOutlivesItsPublisherUntilRemoved) {
  const UniqueChannel channel;
  static_cast<void>(Publisher::create(channel.name(), kConfig));
  EXPECT_FALSE(error_of([&] {
    static_cast<void>(platform::ShmSegment::open(channel.name(), platform::Access::kReadOnly));
  }));
  revenant::remove_channel(channel.name());
  revenant::remove_channel(channel.name());  // removing a missing channel is not an error
  EXPECT_EQ(
      error_of([&] {
        static_cast<void>(platform::ShmSegment::open(channel.name(), platform::Access::kReadOnly));
      }),
      errc::channel_not_found);
}

#ifndef NDEBUG
TEST(PublisherDeathTest, PayloadLargerThanCapacityViolatesTheContract) {
  const UniqueChannel channel;
  Publisher publisher = Publisher::create(channel.name(), kConfig);
  const std::array<std::byte, kCapacity + 1> too_big{};
  EXPECT_DEATH(publisher.publish(too_big), "assertion failed");
}
#endif

}  // namespace

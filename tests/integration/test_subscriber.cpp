#include <revenant/channel/publisher.hpp>
#include <revenant/channel/subscriber.hpp>
#include <revenant/core/layout.hpp>
#include <revenant/errors.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
using revenant::Publisher;
using revenant::ReadResult;
using revenant::ReadStatus;
using revenant::Subscriber;
using revenant::SubscriberConfig;
using revenant::testing::UniqueChannel;

constexpr ChannelConfig kConfig{.slot_size = 128, .slot_count = 8};
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

void publish_payload(Publisher& publisher) {
  std::array<std::byte, kCapacity> payload{};
  const std::uint64_t seq = publisher.last_sequence() + 1;
  const std::size_t length =
      revenant::testing::make_payload(seq, publisher.epoch(), kCapacity, payload);
  publisher.publish(std::span{payload}.first(length));
}

class SubscriberTest : public ::testing::Test {
 protected:
  UniqueChannel channel_;
  std::array<std::byte, kCapacity> buffer_{};

  ReadResult read(Subscriber& subscriber) { return subscriber.try_read(buffer_); }

  void expect_message(Subscriber& subscriber, std::uint64_t seq, std::uint32_t epoch) {
    const ReadResult r = read(subscriber);
    ASSERT_EQ(r.status, ReadStatus::kMessage) << "expected message " << seq;
    EXPECT_EQ(r.first, seq);
    EXPECT_EQ(r.last, seq);
    EXPECT_EQ(r.epoch, epoch);
    EXPECT_TRUE(
        revenant::testing::check_payload(seq, epoch, kCapacity, std::span{buffer_}.first(r.length)))
        << "INV4: bytes of message " << seq;
  }

  // A second read-write mapping, to forge what a broken or foreign writer leaves behind.
  [[nodiscard]] platform::ShmSegment writable() const {
    platform::ShmSegment segment =
        platform::ShmSegment::open(channel_.name(), platform::Access::kReadWrite);
    segment.map(platform::Access::kReadWrite);
    return segment;
  }
};

TEST_F(SubscriberTest, AttachingToAMissingChannelIsChannelNotFound) {
  EXPECT_EQ(error_of([&] { static_cast<void>(Subscriber::attach(channel_.name())); }),
            errc::channel_not_found);
}

TEST_F(SubscriberTest, InvalidConfigIsRejected) {
  const Publisher publisher = Publisher::create(channel_.name(), kConfig);
  EXPECT_EQ(error_of([&] {
              static_cast<void>(Subscriber::attach(channel_.name(), {.attach_timeout = -1s}));
            }),
            errc::invalid_config);
}

TEST_F(SubscriberTest, NothingPublishedYetIsEmpty) {
  const Publisher publisher = Publisher::create(channel_.name(), kConfig);
  Subscriber subscriber = Subscriber::attach(channel_.name());
  EXPECT_EQ(subscriber.next_sequence(), 1U);
  EXPECT_EQ(subscriber.payload_capacity(), kCapacity);
  EXPECT_EQ(read(subscriber).status, ReadStatus::kEmpty);
}

TEST_F(SubscriberTest, MessagesArriveInOrderWithExactBytes) {
  Publisher publisher = Publisher::create(channel_.name(), kConfig);
  Subscriber subscriber = Subscriber::attach(channel_.name());
  for (int i = 0; i < 6; ++i) {
    publish_payload(publisher);
  }
  for (std::uint64_t seq = 1; seq <= 6; ++seq) {
    expect_message(subscriber, seq, 1);
  }
  EXPECT_EQ(read(subscriber).status, ReadStatus::kEmpty);
  EXPECT_EQ(subscriber.next_sequence(), 7U);
}

TEST_F(SubscriberTest, JoinsAtTheLiveEdge) {
  Publisher publisher = Publisher::create(channel_.name(), kConfig);
  for (int i = 0; i < 3; ++i) {
    publish_payload(publisher);
  }
  Subscriber subscriber = Subscriber::attach(channel_.name());
  EXPECT_EQ(subscriber.next_sequence(), 4U);
  EXPECT_EQ(read(subscriber).status, ReadStatus::kEmpty);
  publish_payload(publisher);
  expect_message(subscriber, 4, 1);
}

// Twenty messages through eight slots: slot 1 now holds 17. The subscriber resumes half a ring
// behind head (20 + 1 - 4 = 17) and reports everything before that as one exact gap.
TEST_F(SubscriberTest, INV5_LappedSubscriberGetsTheExactGapThenContiguousMessages) {
  Publisher publisher = Publisher::create(channel_.name(), kConfig);
  Subscriber subscriber = Subscriber::attach(channel_.name());
  for (int i = 0; i < 20; ++i) {
    publish_payload(publisher);
  }

  const ReadResult gap = read(subscriber);
  ASSERT_EQ(gap.status, ReadStatus::kGap);
  EXPECT_EQ(gap.first, 1U);
  EXPECT_EQ(gap.last, 16U);
  for (std::uint64_t seq = 17; seq <= 20; ++seq) {
    expect_message(subscriber, seq, 1);
  }
  EXPECT_EQ(read(subscriber).status, ReadStatus::kEmpty);
}

TEST_F(SubscriberTest, SlowSubscriberAccountsForEveryMessageExactlyOnce) {
  Publisher publisher = Publisher::create(channel_.name(), kConfig);
  Subscriber subscriber = Subscriber::attach(channel_.name());
  std::uint64_t accounted = 0;
  for (int round = 0; round < 50; ++round) {
    for (int i = 0; i < 1 + round % 13; ++i) {
      publish_payload(publisher);
    }
    for (ReadResult r = read(subscriber); r.status != ReadStatus::kEmpty; r = read(subscriber)) {
      ASSERT_EQ(r.first, accounted + 1) << "INV5: nothing skipped, nothing repeated";
      accounted = r.last;
    }
  }
  EXPECT_EQ(accounted, publisher.last_sequence());
}

TEST_F(SubscriberTest, INV7_CorruptSlotLengthIsAOneMessageGap) {
  Publisher publisher = Publisher::create(channel_.name(), kConfig);
  Subscriber subscriber = Subscriber::attach(channel_.name());
  publish_payload(publisher);
  {
    const platform::ShmSegment segment = writable();
    const std::uint64_t forged = core::pack_meta(1, kCapacity + 1);
    std::memcpy(core::slot_at(segment.bytes().data(), kConfig.geometry(), 1) + 8, &forged,
                sizeof forged);
  }
  const ReadResult r = read(subscriber);
  EXPECT_EQ(r.status, ReadStatus::kGap);
  EXPECT_EQ(r.first, 1U);
  EXPECT_EQ(r.last, 1U);
  publish_payload(publisher);
  expect_message(subscriber, 2, 1);
}

TEST_F(SubscriberTest, ForeignOrIncompatibleSegmentsAreRefused) {
  { const Publisher publisher = Publisher::create(channel_.name(), kConfig); }
  {
    const platform::ShmSegment segment = writable();
    core::header_of(segment.bytes().data()).layout_hash ^= 1;
  }
  EXPECT_EQ(error_of([&] { static_cast<void>(Subscriber::attach(channel_.name())); }),
            errc::layout_mismatch);
  {
    const platform::ShmSegment segment = writable();
    core::header_of(segment.bytes().data()).magic = 0x464C457F;
  }
  EXPECT_EQ(error_of([&] { static_cast<void>(Subscriber::attach(channel_.name())); }),
            errc::bad_magic);
}

TEST_F(SubscriberTest, NeverInitialisedSegmentIsIncompleteAfterTheAttachTimeout) {
  platform::ShmSegment leftover = platform::ShmSegment::open_or_create(channel_.name());
  leftover.reserve(core::segment_size(kConfig.geometry()));

  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(error_of([&] {
              static_cast<void>(Subscriber::attach(channel_.name(), {.attach_timeout = 30ms}));
            }),
            errc::segment_incomplete);
  EXPECT_GE(std::chrono::steady_clock::now() - start, 30ms);
}

// The subscriber may map the abandoned file while the publisher re-initialises it in place.
TEST_F(SubscriberTest, AttachWaitsForAPublisherThatIsStillInitialising) {
  {
    platform::ShmSegment leftover = platform::ShmSegment::open_or_create(channel_.name());
    leftover.reserve(core::segment_size(kConfig.geometry()));
  }
  std::thread late_publisher{[&] {
    std::this_thread::sleep_for(30ms);
    Publisher publisher = Publisher::create(channel_.name(), kConfig);
    publish_payload(publisher);
  }};
  Subscriber subscriber = Subscriber::attach(channel_.name(), {.attach_timeout = 10s});
  late_publisher.join();
  EXPECT_GE(subscriber.next_sequence(), 1U);
}

TEST_F(SubscriberTest, MovedSubscriberKeepsReading) {
  Publisher publisher = Publisher::create(channel_.name(), kConfig);
  Subscriber original = Subscriber::attach(channel_.name());
  Subscriber moved{std::move(original)};
  publish_payload(publisher);
  expect_message(moved, 1, 1);
}

#ifndef NDEBUG
TEST_F(SubscriberTest, BufferSmallerThanCapacityViolatesTheContract) {
  const Publisher publisher = Publisher::create(channel_.name(), kConfig);
  Subscriber subscriber = Subscriber::attach(channel_.name());
  std::array<std::byte, kCapacity - 1> small{};
  EXPECT_DEATH(static_cast<void>(subscriber.try_read(small)), "assertion failed");
}
#endif

}  // namespace

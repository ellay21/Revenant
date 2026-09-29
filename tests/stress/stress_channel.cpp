#include <revenant/revenant.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "support/payload.hpp"
#include "support/stress.hpp"
#include "support/unique_channel.hpp"

// End to end over real shared memory: one publisher thread races four subscriber threads through
// a 64-slot ring. Every result each subscriber sees must describe an exact prefix of the stream:
// messages in order with exact bytes (INV4), gaps that start where it left off (INV5), and no
// epoch change or death while the single publisher is alive.

namespace {

namespace testing = revenant::testing;
using revenant::ReadResult;
using revenant::ReadStatus;

constexpr revenant::ChannelConfig kConfig{.slot_size = 128, .slot_count = 64};
constexpr std::uint32_t kCapacity = kConfig.payload_capacity();
constexpr std::size_t kSubscribers = 4;

struct Tally {
  std::uint64_t received = 0;
  std::uint64_t gaps = 0;
  std::uint64_t next = 1;
  std::string failure;
};

void check_stream(revenant::Subscriber& subscriber, std::uint64_t messages, Tally& tally) {
  std::array<std::byte, kCapacity> buffer{};
  std::uint64_t idle = 0;
  while (tally.next <= messages) {
    const ReadResult r = subscriber.try_read(buffer);
    switch (r.status) {
      case ReadStatus::kMessage:
        if (r.first != tally.next || r.epoch != 1 ||
            !testing::check_payload(r.first, 1, kCapacity, std::span{buffer}.first(r.length))) {
          tally.failure = "INV4/INV5: bad message at " + std::to_string(tally.next);
          return;
        }
        ++tally.received;
        ++tally.next;
        break;
      case ReadStatus::kGap:
        if (r.first != tally.next || r.last < r.first) {
          tally.failure = "INV5: gap does not start at " + std::to_string(tally.next);
          return;
        }
        tally.gaps += r.last - r.first + 1;
        tally.next = r.last + 1;
        break;
      case ReadStatus::kEmpty:
        if (++idle % 64 == 0) {
          std::this_thread::yield();
        }
        break;
      case ReadStatus::kEpochChange:
      case ReadStatus::kPublisherDead:
        tally.failure = "unexpected epoch change or death at " + std::to_string(tally.next);
        return;
    }
  }
}

TEST(StressChannel, EverySubscriberSeesAnExactPrefixOfTheStream) {
  const std::uint64_t messages = testing::stress_message_count();
  const testing::UniqueChannel channel;
  revenant::Publisher publisher = revenant::Publisher::create(channel.name(), kConfig);

  std::vector<revenant::Subscriber> subscribers;
  for (std::size_t i = 0; i < kSubscribers; ++i) {
    subscribers.push_back(revenant::Subscriber::attach(channel.name()));
  }
  std::array<Tally, kSubscribers> tallies{};
  std::latch start{static_cast<std::ptrdiff_t>(kSubscribers) + 1};

  std::thread writer{[&] {
    std::array<std::byte, kCapacity> payload{};
    start.arrive_and_wait();
    for (std::uint64_t seq = 1; seq <= messages; ++seq) {
      const std::size_t length = testing::make_payload(seq, 1, kCapacity, payload);
      publisher.publish(std::span{payload}.first(length));
    }
  }};
  std::vector<std::thread> readers;
  for (std::size_t i = 0; i < kSubscribers; ++i) {
    readers.emplace_back([&, i] {
      start.arrive_and_wait();
      check_stream(subscribers[i], messages, tallies[i]);
    });
  }
  writer.join();
  for (std::thread& t : readers) {
    t.join();
  }

  for (const Tally& tally : tallies) {
    EXPECT_EQ(tally.failure, "");
    EXPECT_EQ(tally.received + tally.gaps, messages);
    EXPECT_GT(tally.received, 0U);
  }
}

}  // namespace

#include <revenant/revenant.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "support/child_process.hpp"
#include "support/payload.hpp"
#include "support/stream_checker.hpp"
#include "support/unique_channel.hpp"

// Hot standby: several processes block in Publisher::create(kWaitForLease). Every time the active
// publisher is SIGKILLed, the kernel hands the OFD lease to exactly one of them (INV1), which
// recovers and continues the stream under the next epoch. The subscriber must see one seamless
// stream: each epoch announced once, then its messages, with no sequence delivered twice.

namespace {

namespace testing = revenant::testing;
using namespace std::chrono_literals;
using revenant::LeaseMode;
using revenant::Publisher;
using revenant::ReadResult;
using revenant::ReadStatus;
using revenant::Subscriber;
using testing::ChildLink;
using testing::ChildProcess;

constexpr revenant::ChannelConfig kConfig{.slot_size = 128, .slot_count = 64};
constexpr std::uint32_t kCapacity = kConfig.payload_capacity();
constexpr std::size_t kStandbys = 4;
constexpr int kMessagesPerEpoch = 3;

// Publishes a few messages, reports its epoch, then idles until it is killed.
int serve(Publisher publisher, const ChildLink& link) {
  std::array<std::byte, kCapacity> payload{};
  for (int i = 0; i < kMessagesPerEpoch; ++i) {
    const std::uint64_t seq = publisher.last_sequence() + 1;
    const std::size_t length = testing::make_payload(seq, publisher.epoch(), kCapacity, payload);
    publisher.publish(std::span{payload}.first(length));
  }
  link.send(publisher.epoch());
  static_cast<void>(link.wait_go());
  return 0;
}

void read_until(Subscriber& subscriber, testing::StreamChecker& checker, std::uint64_t next) {
  std::array<std::byte, kCapacity> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (checker.next() < next && std::chrono::steady_clock::now() < deadline) {
    const ReadResult r = subscriber.try_read(buffer);
    if (!checker.check(r, buffer)) {
      return;
    }
  }
}

TEST(Takeover, EachDeathPromotesExactlyOneStandby) {
  const testing::UniqueChannel channel;
  std::optional<ChildProcess> active = ChildProcess::spawn(
      [&](ChildLink& link) { return serve(Publisher::create(channel.name(), kConfig), link); });
  ASSERT_EQ(active->receive(10s), 1U);

  Subscriber subscriber = Subscriber::attach(channel.name(), {.liveness_probe_interval = 1ms});
  testing::StreamChecker checker{subscriber.next_sequence(), subscriber.epoch(), kCapacity};

  std::vector<ChildProcess> standbys;
  for (std::size_t i = 0; i < kStandbys; ++i) {
    standbys.push_back(ChildProcess::spawn([&](ChildLink& link) {
      return serve(Publisher::create(channel.name(), kConfig, LeaseMode::kWaitForLease), link);
    }));
  }
  std::vector<bool> promoted(kStandbys, false);
  for (std::size_t i = 0; i < kStandbys; ++i) {
    EXPECT_FALSE(standbys[i].receive(20ms)) << "a standby ran while the lease was held";
  }

  ChildProcess* current = &*active;
  for (std::uint32_t epoch = 2; epoch <= kStandbys + 1; ++epoch) {
    current->kill();
    ASSERT_TRUE(testing::killed_by(current->wait_for(10s), SIGKILL));

    // Exactly one standby is promoted, and it reports the next epoch.
    std::size_t promoted_now = kStandbys;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (promoted_now == kStandbys && std::chrono::steady_clock::now() < deadline) {
      for (std::size_t i = 0; i < kStandbys && promoted_now == kStandbys; ++i) {
        if (!promoted[i]) {
          if (const auto reported = standbys[i].receive(2ms)) {
            EXPECT_EQ(reported.value_or(0), epoch);
            promoted_now = i;
          }
        }
      }
    }
    ASSERT_LT(promoted_now, kStandbys) << "no standby took over epoch " << epoch;
    promoted[promoted_now] = true;
    for (std::size_t i = 0; i < kStandbys; ++i) {
      if (!promoted[i]) {
        EXPECT_FALSE(standbys[i].receive(20ms)) << "INV1: two standbys took over";
      }
    }

    read_until(subscriber, checker, std::uint64_t{epoch} * kMessagesPerEpoch + 1);
    ASSERT_EQ(checker.failure(), "");
    EXPECT_EQ(checker.epoch(), epoch);
    current = &standbys[promoted_now];
  }

  EXPECT_EQ(checker.epoch_changes(), kStandbys);
  // Attached at the live edge, after the first publisher's messages.
  EXPECT_EQ(checker.received(), kStandbys * kMessagesPerEpoch);
  EXPECT_EQ(checker.gaps(), 0U);
}

}  // namespace

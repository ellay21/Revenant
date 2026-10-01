#include <revenant/fault.hpp>
#include <revenant/revenant.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <system_error>
#include <tuple>

#include "support/child_process.hpp"
#include "support/payload.hpp"
#include "support/stream_checker.hpp"
#include "support/unique_channel.hpp"

// The failure-semantics table, proven with real SIGKILLs. A forked publisher is killed at a
// fault point on its Nth hit; the subscriber must then see an exact prefix of the stream, learn
// of the death, and continue seamlessly with the next publisher's epoch.

namespace {

namespace testing = revenant::testing;
using namespace std::chrono_literals;
using revenant::ChannelConfig;
using revenant::Publisher;
using revenant::ReadResult;
using revenant::ReadStatus;
using revenant::Subscriber;
using testing::ChildLink;
using testing::ChildProcess;
using testing::FaultPoint;

constexpr ChannelConfig kConfig{.slot_size = 128, .slot_count = 8};
constexpr std::uint32_t kCapacity = kConfig.payload_capacity();
constexpr revenant::SubscriberConfig kFastProbe{.liveness_probe_interval = 1ms};

void publish_payload(Publisher& publisher) {
  std::array<std::byte, kCapacity> payload{};
  const std::uint64_t seq = publisher.last_sequence() + 1;
  const std::size_t length = testing::make_payload(seq, publisher.epoch(), kCapacity, payload);
  publisher.publish(std::span{payload}.first(length));
}

// Reads until `until` is returned (or a deadline passes), checking every result on the way.
ReadResult drain(Subscriber& subscriber, testing::StreamChecker& checker, ReadStatus until) {
  std::array<std::byte, kCapacity> buffer{};
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    const ReadResult r = subscriber.try_read(buffer);
    if (!checker.check(r, buffer) || r.status == until ||
        std::chrono::steady_clock::now() >= deadline) {
      return r;
    }
  }
}

const char* name_of(FaultPoint point) {
  switch (point) {
    case FaultPoint::kAfterClaim:
      return "F1_AfterClaim";
    case FaultPoint::kMidPayload:
      return "F2_MidPayload";
    case FaultPoint::kBeforeCommit:
      return "F3_BeforeCommit";
    case FaultPoint::kAfterCommit:
      return "F4_AfterCommit";
    case FaultPoint::kAfterHead:
      return "F5_AfterHead";
    default:
      return "other";
  }
}

class KillPoint : public ::testing::TestWithParam<std::tuple<FaultPoint, std::uint64_t>> {};

// Killed while publishing message n. F1-F3 never committed it, so the next publisher reuses n;
// F4 committed it without advancing head, and F5 completed it, so the stream ends at n.
TEST_P(KillPoint, SubscriberSeesAnExactPrefixThenTheNextEpoch) {
  const FaultPoint point = std::get<0>(GetParam());
  const std::uint64_t hit = std::get<1>(GetParam());
  const bool committed = point == FaultPoint::kAfterCommit || point == FaultPoint::kAfterHead;
  const std::uint64_t last_published = committed ? hit : hit - 1;

  const testing::UniqueChannel channel;
  ChildProcess child = ChildProcess::spawn([&](ChildLink& link) {
    Publisher publisher = Publisher::create(channel.name(), kConfig);
    link.send(1);
    if (!link.wait_go()) {
      return 2;
    }
    testing::arm_fault(point, hit);
    for (std::uint64_t i = 0; i < hit + 100; ++i) {
      publish_payload(publisher);
    }
    return 3;  // the fault point never fired
  });
  ASSERT_TRUE(child.receive(10s));
  Subscriber subscriber = Subscriber::attach(channel.name(), kFastProbe);
  testing::StreamChecker checker{subscriber.next_sequence(), subscriber.epoch(), kCapacity};
  child.go();
  ASSERT_TRUE(testing::killed_by(child.wait_for(10s), SIGKILL));

  // G1, G2, G3: an exact prefix up to the last committed message, then the death.
  EXPECT_EQ(drain(subscriber, checker, ReadStatus::kPublisherDead).status,
            ReadStatus::kPublisherDead)
      << checker.failure();
  EXPECT_EQ(checker.next(), last_published + 1) << checker.failure();

  // G4, INV3: the next publisher resumes exactly after the last committed message.
  Publisher next = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(next.last_sequence(), last_published);
  EXPECT_EQ(next.epoch(), 2U);
  for (int i = 0; i < 3; ++i) {
    publish_payload(next);
  }

  // INV5, INV6: the new epoch is announced first, and no sequence is delivered twice.
  while (checker.next() <= last_published + 3 && checker.failure().empty()) {
    if (drain(subscriber, checker, ReadStatus::kMessage).status != ReadStatus::kMessage) {
      break;
    }
  }
  EXPECT_EQ(checker.failure(), "");
  EXPECT_EQ(checker.epoch(), 2U);
  EXPECT_EQ(checker.epoch_changes(), 1U);
  EXPECT_EQ(checker.next(), last_published + 4);
}

INSTANTIATE_TEST_SUITE_P(
    PublishPath, KillPoint,
    ::testing::Combine(::testing::Values(FaultPoint::kAfterClaim, FaultPoint::kMidPayload,
                                         FaultPoint::kBeforeCommit, FaultPoint::kAfterCommit,
                                         FaultPoint::kAfterHead),
                       // first message, second, last slot of the first lap, first of the
                       // second lap, and deep into the third lap
                       ::testing::Values(std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{8},
                                         std::uint64_t{9}, std::uint64_t{21})),
    [](const auto& test) {
      return std::string{name_of(std::get<0>(test.param))} + "_Hit" +
             std::to_string(std::get<1>(test.param));
    });

// F6: dying during recovery leaves state the next publisher recovers identically.
TEST(KillPointRecovery, F6_DyingDuringRecoveryIsIdempotent) {
  const testing::UniqueChannel channel;
  {
    Publisher first = Publisher::create(channel.name(), kConfig);
    for (int i = 0; i < 5; ++i) {
      publish_payload(first);
    }
  }
  Subscriber subscriber = Subscriber::attach(channel.name(), kFastProbe);
  testing::StreamChecker checker{subscriber.next_sequence(), subscriber.epoch(), kCapacity};

  ChildProcess recovering = ChildProcess::spawn([&](ChildLink&) {
    testing::arm_fault(FaultPoint::kDuringRecovery, 1);
    static_cast<void>(Publisher::create(channel.name(), kConfig));
    return 3;
  });
  ASSERT_TRUE(testing::killed_by(recovering.wait_for(10s), SIGKILL));

  Publisher next = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(next.last_sequence(), 5U);
  EXPECT_EQ(next.epoch(), 2U) << "the dead recoverer never bumped the epoch";
  publish_payload(next);
  while (checker.received() == 0 && checker.failure().empty()) {
    if (drain(subscriber, checker, ReadStatus::kMessage).status != ReadStatus::kMessage) {
      break;
    }
  }
  EXPECT_EQ(checker.failure(), "");
  EXPECT_EQ(checker.next(), 7U);
}

// F7: a creator killed before publishing magic leaves a retryable, re-initialisable segment.
TEST(KillPointRecovery, F7_DyingDuringInitialisationIsRetryable) {
  const testing::UniqueChannel channel;
  ChildProcess creator = ChildProcess::spawn([&](ChildLink&) {
    testing::arm_fault(FaultPoint::kDuringInit, 1);
    static_cast<void>(Publisher::create(channel.name(), kConfig));
    return 3;
  });
  ASSERT_TRUE(testing::killed_by(creator.wait_for(10s), SIGKILL));

  try {
    static_cast<void>(Subscriber::attach(channel.name(), {.attach_timeout = 20ms}));
    ADD_FAILURE() << "attached to a half-initialised segment";
  } catch (const std::system_error& e) {
    EXPECT_EQ(e.code(), revenant::errc::segment_incomplete);
  }

  Publisher publisher = Publisher::create(channel.name(), kConfig);
  EXPECT_EQ(publisher.epoch(), 1U);
  EXPECT_EQ(publisher.last_sequence(), 0U);
  EXPECT_NO_THROW(static_cast<void>(Subscriber::attach(channel.name())));
}

// Nothing waits for subscribers, so killing one mid-read changes nothing for anyone else.
TEST(KillSubscriber, SubscriberDeathNeverAffectsThePublisherOrOtherSubscribers) {
  const testing::UniqueChannel channel;
  Publisher publisher = Publisher::create(channel.name(), kConfig);
  Subscriber survivor = Subscriber::attach(channel.name());
  testing::StreamChecker checker{survivor.next_sequence(), survivor.epoch(), kCapacity};

  ChildProcess reader = ChildProcess::spawn([&](ChildLink& link) -> int {
    Subscriber doomed = Subscriber::attach(channel.name());
    std::array<std::byte, kCapacity> buffer{};
    link.send(1);
    for (;;) {
      static_cast<void>(doomed.try_read(buffer));
    }
  });
  ASSERT_TRUE(reader.receive(10s));
  for (int i = 0; i < 4; ++i) {
    publish_payload(publisher);
  }
  reader.kill();
  ASSERT_TRUE(testing::killed_by(reader.wait_for(10s), SIGKILL));
  for (int i = 0; i < 4; ++i) {
    publish_payload(publisher);
  }

  std::array<std::byte, kCapacity> buffer{};
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(checker.check(survivor.try_read(buffer), buffer)) << checker.failure();
  }
  EXPECT_EQ(checker.received(), 8U);
}

}  // namespace

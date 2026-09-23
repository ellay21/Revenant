#include <revenant/platform/lease.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <optional>

#include "support/child_process.hpp"
#include "support/unique_channel.hpp"

// The publisher lease is an open-file-description (OFD) lock. These tests pin the semantics
// that make it a correct single-writer lease, and that POSIX record locks would get wrong.

namespace {

using namespace std::chrono_literals;
using revenant::platform::Access;
using revenant::platform::lease_is_held;
using revenant::platform::ShmSegment;
using revenant::platform::try_acquire_lease;
using revenant::testing::ChildLink;
using revenant::testing::ChildProcess;
using revenant::testing::UniqueChannel;

ShmSegment make_segment(const UniqueChannel& channel) {
  ShmSegment segment = ShmSegment::open_or_create(channel.name());
  segment.reserve(4096);
  return segment;
}

// A child that opens the channel, takes the lease, reports 1 (or 0 on failure), then waits.
ChildProcess spawn_holder(const UniqueChannel& channel) {
  return ChildProcess::spawn([&channel](const ChildLink& link) {
    const ShmSegment segment = ShmSegment::open_or_create(channel.name());
    link.send(try_acquire_lease(segment.fd()) ? 1 : 0);
    return link.wait_go() ? 0 : 1;
  });
}

// POSIX F_SETLK would let this succeed: record locks belong to the process, not the descriptor.
TEST(Lease, INV1_SecondDescriptionInTheSameProcessCannotAcquire) {
  const UniqueChannel channel;
  const ShmSegment first = make_segment(channel);
  const ShmSegment second = ShmSegment::open_or_create(channel.name());

  EXPECT_TRUE(try_acquire_lease(first.fd()));
  EXPECT_FALSE(try_acquire_lease(second.fd()));
  EXPECT_TRUE(try_acquire_lease(first.fd())) << "re-locking through the holder is idempotent";
}

TEST(Lease, ReadOnlyDescriptorProbesWithoutAcquiring) {
  const UniqueChannel channel;
  const ShmSegment holder = make_segment(channel);
  const ShmSegment probe = ShmSegment::open(channel.name(), Access::kReadOnly);

  EXPECT_FALSE(lease_is_held(probe.fd()));
  ASSERT_TRUE(try_acquire_lease(holder.fd()));
  EXPECT_TRUE(lease_is_held(probe.fd()));
  EXPECT_TRUE(lease_is_held(probe.fd())) << "probing twice must not change anything";
}

// The classic POSIX-lock bug: closing any descriptor to the file drops the process's locks.
TEST(Lease, ClosingAnUnrelatedDescriptorDoesNotRelease) {
  const UniqueChannel channel;
  const ShmSegment holder = make_segment(channel);
  const ShmSegment probe = ShmSegment::open(channel.name(), Access::kReadOnly);
  ASSERT_TRUE(try_acquire_lease(holder.fd()));
  {
    const ShmSegment unrelated = ShmSegment::open_or_create(channel.name());
    const ShmSegment another_reader = ShmSegment::open(channel.name(), Access::kReadOnly);
  }
  EXPECT_TRUE(lease_is_held(probe.fd()));
}

TEST(Lease, ClosingTheHolderReleasesIt) {
  const UniqueChannel channel;
  std::optional<ShmSegment> holder{make_segment(channel)};
  const ShmSegment probe = ShmSegment::open(channel.name(), Access::kReadOnly);
  ASSERT_TRUE(try_acquire_lease(holder->fd()));

  holder.reset();
  EXPECT_FALSE(lease_is_held(probe.fd()));
  const ShmSegment next = ShmSegment::open_or_create(channel.name());
  EXPECT_TRUE(try_acquire_lease(next.fd()));
}

TEST(Lease, OtherProcessSeesTheLeaseUntilTheHolderExits) {
  const UniqueChannel channel;
  const ShmSegment probe = make_segment(channel);
  ChildProcess holder = spawn_holder(channel);
  ASSERT_EQ(holder.receive(5s), 1U);

  EXPECT_TRUE(lease_is_held(probe.fd()));
  EXPECT_FALSE(try_acquire_lease(probe.fd()));

  holder.go();
  EXPECT_TRUE(revenant::testing::exited_with(holder.wait_for(5s), 0));
  EXPECT_FALSE(lease_is_held(probe.fd()));
  EXPECT_TRUE(try_acquire_lease(probe.fd()));
}

TEST(Lease, KernelReleasesTheLeaseOfAKilledHolder) {
  const UniqueChannel channel;
  const ShmSegment probe = make_segment(channel);
  ChildProcess holder = spawn_holder(channel);
  ASSERT_EQ(holder.receive(5s), 1U);
  ASSERT_TRUE(lease_is_held(probe.fd()));

  holder.kill(SIGKILL);
  EXPECT_TRUE(revenant::testing::killed_by(holder.wait_for(5s), SIGKILL));
  EXPECT_FALSE(lease_is_held(probe.fd()));
  EXPECT_TRUE(try_acquire_lease(probe.fd()));
}

}  // namespace

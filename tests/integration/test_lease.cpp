#include <revenant/platform/lease.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <gtest/gtest.h>

#include <signal.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <optional>
#include <thread>

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

// kill(pid, 0) reports a zombie as alive; the lease does not, because a dying process closes
// its descriptors before it becomes a zombie.
TEST(Lease, ZombieHolderHasAlreadyReleasedIt) {
  const UniqueChannel channel;
  const ShmSegment probe = make_segment(channel);
  ChildProcess holder = spawn_holder(channel);
  ASSERT_EQ(holder.receive(5s), 1U);

  holder.kill(SIGKILL);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (lease_is_held(probe.fd()) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_FALSE(lease_is_held(probe.fd()));
  ASSERT_EQ(::kill(holder.pid(), 0), 0) << "the unreaped child must still exist, as a zombie";
  EXPECT_TRUE(revenant::testing::killed_by(holder.wait_for(5s), SIGKILL));
}

TEST(Lease, BlockingAcquireReturnsAsSoonAsTheHolderDies) {
  const UniqueChannel channel;
  const ShmSegment standby = make_segment(channel);
  ChildProcess holder = spawn_holder(channel);
  ASSERT_EQ(holder.receive(5s), 1U);

  std::atomic<bool> acquired{false};
  std::thread waiter{[&] {
    revenant::platform::acquire_lease_blocking(standby.fd());
    acquired.store(true);
  }};
  std::this_thread::sleep_for(50ms);  // the behaviour under test: still blocked while held
  EXPECT_FALSE(acquired.load());

  holder.kill(SIGKILL);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!acquired.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_TRUE(acquired.load());
  waiter.join();
  EXPECT_TRUE(revenant::testing::killed_by(holder.wait_for(5s), SIGKILL));
}

TEST(Lease, BlockingAcquireOfAFreeLeaseReturnsImmediately) {
  const UniqueChannel channel;
  const ShmSegment segment = make_segment(channel);
  revenant::platform::acquire_lease_blocking(segment.fd());
  const ShmSegment probe = ShmSegment::open(channel.name(), Access::kReadOnly);
  EXPECT_TRUE(lease_is_held(probe.fd()));
}

// Documented caveat: fork without exec shares the open file description, and with it the lease.
TEST(Lease, ChildForkedAfterAcquiringKeepsTheLeaseAlive) {
  const UniqueChannel channel;
  std::optional<ShmSegment> holder{make_segment(channel)};
  const ShmSegment probe = ShmSegment::open(channel.name(), Access::kReadOnly);
  ASSERT_TRUE(try_acquire_lease(holder->fd()));

  ChildProcess child =
      ChildProcess::spawn([](const ChildLink& link) { return link.wait_go() ? 0 : 1; });
  holder.reset();
  EXPECT_TRUE(lease_is_held(probe.fd())) << "the child's inherited descriptor still holds it";

  child.go();
  EXPECT_TRUE(revenant::testing::exited_with(child.wait_for(5s), 0));
  EXPECT_FALSE(lease_is_held(probe.fd()));
}

}  // namespace

#include <revenant/fault.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdint>

#include "support/child_process.hpp"

namespace {

using namespace std::chrono_literals;
using revenant::testing::ChildLink;
using revenant::testing::ChildProcess;
using revenant::testing::FaultPoint;

// The child reports each hit before taking it, so the parent knows exactly which one killed it.
ChildProcess hit_repeatedly(FaultPoint armed, std::uint64_t kill_on, FaultPoint hit) {
  return ChildProcess::spawn([=](ChildLink& link) {
    revenant::testing::arm_fault(armed, kill_on);
    for (std::uint64_t n = 1; n <= 10; ++n) {
      link.send(n);
      revenant::testing::fault_point_reached(hit);
    }
    return 0;
  });
}

std::uint64_t hits_seen(const ChildProcess& child) {
  std::uint64_t last = 0;
  while (const auto n = child.receive(2s)) {
    last = *n;
  }
  return last;
}

TEST(FaultInjection, ArmedPointKillsOnExactlyTheNthHit) {
  for (const std::uint64_t n : {std::uint64_t{1}, std::uint64_t{3}, std::uint64_t{7}}) {
    ChildProcess child = hit_repeatedly(FaultPoint::kBeforeCommit, n, FaultPoint::kBeforeCommit);
    EXPECT_EQ(hits_seen(child), n);
    EXPECT_TRUE(revenant::testing::killed_by(child.wait_for(5s), SIGKILL)) << n;
  }
}

TEST(FaultInjection, OtherPointsNeverFire) {
  ChildProcess child = hit_repeatedly(FaultPoint::kAfterHead, 1, FaultPoint::kMidPayload);
  EXPECT_EQ(hits_seen(child), 10U);
  EXPECT_TRUE(revenant::testing::exited_with(child.wait_for(5s), 0));
}

TEST(FaultInjection, UnarmedProcessNeverFires) {
  ChildProcess child = ChildProcess::spawn([](ChildLink&) {
    for (int i = 0; i < 100; ++i) {
      REVENANT_FAULT_POINT(kAfterClaim);
    }
    return 0;
  });
  EXPECT_TRUE(revenant::testing::exited_with(child.wait_for(5s), 0));
}

}  // namespace

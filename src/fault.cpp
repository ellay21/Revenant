#include <revenant/fault.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>

namespace revenant::testing {
namespace {

std::atomic<FaultPoint> armed_point{FaultPoint::kNone};
std::atomic<std::uint64_t> hits_left{0};

}  // namespace

void arm_fault(FaultPoint point, std::uint64_t kill_on_hit) noexcept {
  hits_left.store(kill_on_hit);
  armed_point.store(point);
}

void fault_point_reached(FaultPoint point) noexcept {
  if (point != armed_point.load()) {
    return;
  }
  if (hits_left.fetch_sub(1) == 1) {
    std::raise(SIGKILL);
  }
}

}  // namespace revenant::testing

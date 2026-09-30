#pragma once

#include <cstdint>

// Fault injection for the crash suite. REVENANT_FAULT_POINT marks the protocol steps where a
// publisher can die. In builds with REVENANT_FAULT_INJECTION (dev, asan, tsan) a point armed in a
// forked test child raises a real SIGKILL on its Nth hit, so the kernel tears the process down
// exactly as an OOM kill would. In every other build the macro is nothing and this namespace
// contributes no code at all. The library never arms a point itself.

namespace revenant::testing {

enum class FaultPoint : std::uint8_t {
  kNone,
  kAfterClaim,      ///< F1: seq word is odd, nothing else written.
  kMidPayload,      ///< F2: half the payload written.
  kBeforeCommit,    ///< F3: payload complete, seq word still odd.
  kAfterCommit,     ///< F4: committed, head not yet advanced.
  kAfterHead,       ///< F5: message fully published.
  kDuringRecovery,  ///< F6: head recovered, epoch not yet bumped.
  kDuringInit,      ///< F7: header written, magic not yet published.
};

#if defined(REVENANT_FAULT_INJECTION)

/// Kills the calling process with SIGKILL on the `kill_on_hit`-th hit of `point` (1-based).
void arm_fault(FaultPoint point, std::uint64_t kill_on_hit) noexcept;

void fault_point_reached(FaultPoint point) noexcept;

#define REVENANT_FAULT_POINT(point) \
  ::revenant::testing::fault_point_reached(::revenant::testing::FaultPoint::point)

#else

#define REVENANT_FAULT_POINT(point) static_cast<void>(0)

#endif

}  // namespace revenant::testing

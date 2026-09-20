#pragma once

#include <revenant/platform/shm_segment.hpp>

#include <unistd.h>

#include <atomic>
#include <string>

namespace revenant::testing {

/// A channel name unique across parallel test processes, unlinked on destruction, so no test
/// leaves a segment behind in /dev/shm.
class UniqueChannel {
 public:
  UniqueChannel()
      : name_("t" + std::to_string(::getpid()) + "_" + std::to_string(next_id_.fetch_add(1))) {}
  UniqueChannel(const UniqueChannel&) = delete;
  UniqueChannel& operator=(const UniqueChannel&) = delete;
  UniqueChannel(UniqueChannel&&) = delete;
  UniqueChannel& operator=(UniqueChannel&&) = delete;
  ~UniqueChannel() {
    try {
      platform::unlink_channel(name_);
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor must not throw
    }
  }

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

 private:
  inline static std::atomic<int> next_id_{0};
  std::string name_;
};

}  // namespace revenant::testing

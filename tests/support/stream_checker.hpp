#pragma once

#include <revenant/channel/read_result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

#include "support/payload.hpp"

namespace revenant::testing {

/// The subscriber checker shared by the crash tests. Every result must be consistent with an exact
/// prefix of the published stream: messages in order with exact bytes (INV4, INV5), gaps that
/// start where the subscriber left off (INV5), and strictly increasing epochs announced before
/// their messages (INV6). Payloads must come from make_payload(seq, epoch, capacity).
class StreamChecker {
 public:
  StreamChecker(std::uint64_t next, std::uint32_t epoch, std::uint32_t capacity) noexcept
      : next_(next), epoch_(epoch), capacity_(capacity) {}

  /// False on the first violation; failure() says what it was.
  bool check(const ReadResult& r, std::span<const std::byte> buffer) {
    if (!failure_.empty()) {
      return false;
    }
    switch (r.status) {
      case ReadStatus::kMessage:
        if (r.first != next_ || r.epoch != epoch_ ||
            !check_payload(r.first, r.epoch, capacity_, buffer.first(r.length))) {
          return fail("message " + std::to_string(r.first) + " epoch " + std::to_string(r.epoch) +
                      ", expected " + std::to_string(next_) + " epoch " + std::to_string(epoch_));
        }
        ++received_;
        ++next_;
        return true;
      case ReadStatus::kGap:
        if (r.first != next_ || r.last < r.first) {
          return fail("gap " + std::to_string(r.first) + ".." + std::to_string(r.last) +
                      ", expected it to start at " + std::to_string(next_));
        }
        gaps_ += r.last - r.first + 1;
        next_ = r.last + 1;
        return true;
      case ReadStatus::kEpochChange:
        if (r.epoch <= epoch_) {
          return fail("epoch went from " + std::to_string(epoch_) + " to " +
                      std::to_string(r.epoch));
        }
        epoch_ = r.epoch;
        ++epoch_changes_;
        return true;
      case ReadStatus::kPublisherDead:
        ++deaths_;
        return true;
      case ReadStatus::kEmpty:
        return true;
    }
    return fail("unknown status");
  }

  [[nodiscard]] std::uint64_t next() const noexcept { return next_; }
  [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }
  [[nodiscard]] std::uint64_t received() const noexcept { return received_; }
  [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
  [[nodiscard]] std::uint64_t deaths() const noexcept { return deaths_; }
  [[nodiscard]] std::uint64_t epoch_changes() const noexcept { return epoch_changes_; }
  [[nodiscard]] const std::string& failure() const noexcept { return failure_; }

 private:
  bool fail(std::string what) {
    failure_ = std::move(what);
    return false;
  }

  std::uint64_t next_;
  std::uint32_t epoch_;
  std::uint32_t capacity_;
  std::uint64_t received_ = 0;
  std::uint64_t gaps_ = 0;
  std::uint64_t deaths_ = 0;
  std::uint64_t epoch_changes_ = 0;
  std::string failure_;
};

}  // namespace revenant::testing

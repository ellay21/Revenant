#include <revenant/channel/subscriber.hpp>
#include <revenant/core/atomics.hpp>
#include <revenant/core/layout.hpp>
#include <revenant/core/seqlock.hpp>
#include <revenant/errors.hpp>
#include <revenant/platform/lease.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace revenant {
namespace {

[[noreturn]] void fail(std::error_code code, std::string_view channel) {
  throw std::system_error(code, "revenant channel \"" + std::string{channel} + "\"");
}

}  // namespace

Subscriber::Subscriber(platform::ShmSegment segment, core::RingGeometry geometry,
                       std::uint64_t next, std::uint32_t epoch,
                       const SubscriberConfig& config) noexcept
    : segment_(std::move(segment)),
      geometry_(geometry),
      capacity_(core::payload_capacity(geometry)),
      epoch_(epoch),
      next_(next),
      config_(config) {}

Subscriber Subscriber::attach(std::string_view channel, const SubscriberConfig& config) {
  if (const std::error_code ec = validate(config)) {
    fail(ec, channel);
  }
  const auto deadline = std::chrono::steady_clock::now() + config.attach_timeout;
  for (;;) {
    platform::ShmSegment segment = platform::ShmSegment::open(channel, platform::Access::kReadOnly);
    std::error_code error = errc::segment_incomplete;
    // Below one header there is nothing to validate, and mapping would be pointless.
    if (segment.file_size() >= core::kSlotsOffset) {
      segment.map(platform::Access::kReadOnly);
      const core::HeaderCheck check = core::validate_header(segment.bytes());
      error = check.error;
      if (!error) {
        const core::ControlBlock& control = core::control_of(segment.bytes().data());
        const std::uint64_t head = core::atomics::load_acquire(control.head);
        if (head >= core::kMaxSequence) {
          fail(errc::segment_corrupt, channel);
        }
        const std::uint32_t epoch = core::atomics::load_acquire(control.epoch);
        return Subscriber{std::move(segment), check.geometry, head + 1, epoch, config};
      }
    }
    if (error != errc::segment_incomplete || std::chrono::steady_clock::now() >= deadline) {
      fail(error, channel);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

ReadResult Subscriber::try_read(std::span<std::byte> buffer) noexcept {
  REVENANT_ASSERT(buffer.size() >= capacity_);
  const std::byte* const slot = core::slot_at(segment_.bytes().data(), geometry_, next_);
  const core::SlotRead r = core::read_slot(slot, capacity_, next_, buffer);
  switch (r.state) {
    case core::SlotState::kCommitted: {
      // INV6: announce a new publisher before its first message; this read is not consumed.
      if (r.epoch != epoch_) {
        epoch_ = r.epoch;
        return {ReadStatus::kEpochChange, next_, next_, r.epoch};
      }
      dead_reported_ = false;
      const std::uint64_t seq = next_++;
      return {ReadStatus::kMessage, seq, seq, r.epoch, r.length};
    }
    case core::SlotState::kNotYet:
    case core::SlotState::kInProgress:
      return idle();
    case core::SlotState::kLapped:
    case core::SlotState::kTorn:
      return overrun(r.observed_seq);
    case core::SlotState::kCorrupt: {
      const std::uint64_t seq = next_++;  // INV7: skip it, but report it
      return {ReadStatus::kGap, seq, seq};
    }
  }
  return idle();
}

// Only reached with nothing to read, so the extra load and the clock read cost the hot path
// nothing. A restarted publisher bumps the epoch before publishing, so an idle subscriber learns
// of it immediately. Death needs a syscall, so it is rate-limited.
ReadResult Subscriber::idle() noexcept {
  const std::uint32_t epoch =
      core::atomics::load_acquire(core::control_of(segment_.bytes().data()).epoch);
  if (epoch != epoch_) {
    epoch_ = epoch;
    dead_reported_ = false;
    return {ReadStatus::kEpochChange, next_, next_, epoch};
  }

  const auto now = std::chrono::steady_clock::now();
  if (now - last_probe_ >= config_.liveness_probe_interval) {
    last_probe_ = now;
    if (platform::lease_is_held(segment_.fd())) {
      dead_reported_ = false;
    } else if (!dead_reported_) {
      dead_reported_ = true;
      return {ReadStatus::kPublisherDead, next_, next_};
    }
  }
  return {ReadStatus::kEmpty, next_, next_};
}

// Resume half a ring behind the writer: the oldest slot is the next one it overwrites, and the
// newest loses the most data. head may lag the slot we saw by one (INV3), so the slot itself
// bounds it from below. A corrupt head is clamped so a hostile segment cannot overflow next_.
ReadResult Subscriber::overrun(std::uint64_t observed_seq) noexcept {
  const std::uint64_t advertised =
      core::atomics::load_acquire(core::control_of(segment_.bytes().data()).head);
  const std::uint64_t head =
      std::min(std::max(advertised, observed_seq - 1), core::kMaxSequence - 1);
  const std::uint64_t half_ring = geometry_.slot_count / 2;
  const std::uint64_t behind = head + 1 > half_ring ? head + 1 - half_ring : 1;
  const std::uint64_t resume = std::max(next_ + 1, behind);

  const ReadResult gap{ReadStatus::kGap, next_, resume - 1};
  next_ = resume;
  return gap;
}

}  // namespace revenant

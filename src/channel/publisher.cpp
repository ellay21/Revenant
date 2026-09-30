#include <revenant/channel/publisher.hpp>
#include <revenant/core/atomics.hpp>
#include <revenant/core/layout.hpp>
#include <revenant/core/seqlock.hpp>
#include <revenant/errors.hpp>
#include <revenant/fault.hpp>
#include <revenant/platform/lease.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

namespace revenant {
namespace {

[[noreturn]] void fail(std::error_code code, std::string_view channel) {
  throw std::system_error(code, "revenant channel \"" + std::string{channel} + "\"");
}

std::uint64_t unix_now_ns() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void take_lease(const platform::ShmSegment& segment, LeaseMode mode, std::string_view channel) {
  if (mode == LeaseMode::kWaitForLease) {
    platform::acquire_lease_blocking(segment.fd());
  } else if (!platform::try_acquire_lease(segment.fd())) {
    fail(errc::publisher_active, channel);
  }
}

// Maps and validates an existing segment. Returns false when it must be initialised: the file
// is new, or its creator died before publishing magic (F7). Holding the lease, nobody else can
// be initialising it, so an incomplete segment is always abandoned.
bool open_existing(platform::ShmSegment& segment, core::RingGeometry geometry,
                   std::string_view channel) {
  if (segment.file_size() == 0) {
    return false;
  }
  segment.map(platform::Access::kReadWrite);
  const core::HeaderCheck check = core::validate_header(segment.bytes());
  if (check.error == errc::segment_incomplete) {
    segment.unmap();
    return false;
  }
  if (check.error) {
    fail(check.error, channel);
  }
  if (check.geometry != geometry) {
    fail(errc::geometry_mismatch, channel);
  }
  return true;
}

// Rolls head forward over the one message the previous publisher may have committed (W4)
// without advertising (W5). A slot left mid-write was never committed, so no subscriber saw it
// and its sequence is simply reused.
std::uint64_t recover_head(std::byte* base, core::RingGeometry geometry, std::string_view channel) {
  core::ControlBlock& control = core::control_of(base);
  std::uint64_t head = core::atomics::load_acquire(control.head);
  if (head >= core::kMaxSequence) {
    fail(errc::segment_corrupt, channel);
  }
  const std::uint64_t word = core::load_seq_word(core::slot_at(base, geometry, head + 1));
  if (word == core::committed_word(head + 1)) {
    ++head;
    core::atomics::store_release(control.head, head);
  }
  return head;
}

std::uint32_t next_epoch(std::byte* base) noexcept {
  std::uint32_t& stored = core::control_of(base).epoch;
  std::uint32_t epoch = core::atomics::load_relaxed(stored) + 1;
  if (epoch == 0) {
    epoch = 1;  // 0 means "no publisher yet"; skip it if 2^32 restarts ever wrap
  }
  core::atomics::store_release(stored, epoch);
  return epoch;
}

}  // namespace

Publisher::Publisher(platform::ShmSegment segment, core::RingGeometry geometry, std::uint32_t epoch,
                     std::uint64_t next_seq) noexcept
    : segment_(std::move(segment)),
      geometry_(geometry),
      capacity_(core::payload_capacity(geometry)),
      epoch_(epoch),
      next_seq_(next_seq) {}

Publisher Publisher::create(std::string_view channel, const ChannelConfig& config, LeaseMode mode) {
  if (const std::error_code ec = validate(config)) {
    fail(ec, channel);
  }
  platform::ShmSegment segment = platform::ShmSegment::open_or_create(channel);
  take_lease(segment, mode, channel);

  const core::RingGeometry geometry = config.geometry();
  if (!open_existing(segment, geometry, channel)) {
    segment.reserve(core::segment_size(geometry));
    segment.map(platform::Access::kReadWrite);
    core::initialize_segment(segment.bytes(), geometry, unix_now_ns());
  }

  std::byte* const base = segment.bytes().data();
  const std::uint64_t head = recover_head(base, geometry, channel);
  REVENANT_FAULT_POINT(kDuringRecovery);  // F6
  const std::uint32_t epoch = next_epoch(base);
  return Publisher{std::move(segment), geometry, epoch, head + 1};
}

void remove_channel(std::string_view channel) {
  platform::unlink_channel(channel);
}

}  // namespace revenant

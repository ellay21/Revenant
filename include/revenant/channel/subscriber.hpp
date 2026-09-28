#pragma once

#include <revenant/channel/channel_config.hpp>
#include <revenant/channel/read_result.hpp>
#include <revenant/core/ring.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace revenant {

/// One reader of a channel. It maps the segment read-only, so a buggy subscriber can never
/// corrupt the channel for anyone else, and it never slows the publisher: a subscriber that
/// falls behind is told exactly which messages it missed (kGap).
/// Move-only; use from one thread at a time.
class Subscriber {
 public:
  /// Attaches to an existing channel and starts at the live edge: the first message published
  /// after this call. Retries for config.attach_timeout while the segment is incomplete.
  /// Throws std::system_error: invalid_name, invalid_config, channel_not_found,
  /// segment_incomplete, or a validation error (bad_magic, version_mismatch, ...).
  [[nodiscard]] static Subscriber attach(std::string_view channel,
                                         const SubscriberConfig& config = {});

  /// Reads the next message into `buffer`, or reports why there is none. Never blocks.
  /// Narrow contract: buffer.size() >= payload_capacity().
  ReadResult try_read(std::span<std::byte> buffer) noexcept;

  /// Sequence the next kMessage will carry if nothing is missed.
  [[nodiscard]] std::uint64_t next_sequence() const noexcept { return next_; }
  [[nodiscard]] std::uint32_t payload_capacity() const noexcept { return capacity_; }
  [[nodiscard]] core::RingGeometry geometry() const noexcept { return geometry_; }

 private:
  Subscriber(platform::ShmSegment segment, core::RingGeometry geometry, std::uint64_t next,
             const SubscriberConfig& config) noexcept;

  ReadResult overrun(std::uint64_t observed_seq) noexcept;

  platform::ShmSegment segment_;
  core::RingGeometry geometry_;
  std::uint32_t capacity_;
  std::uint64_t next_;
  SubscriberConfig config_;
};

}  // namespace revenant

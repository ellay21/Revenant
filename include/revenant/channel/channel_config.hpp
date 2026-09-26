#pragma once

#include <revenant/core/layout.hpp>
#include <revenant/core/ring.hpp>
#include <revenant/errors.hpp>

#include <chrono>
#include <cstdint>
#include <system_error>

namespace revenant {

/// Shape of a channel: `slot_count` slots of `slot_size` bytes. Every message must fit in one
/// slot, so the payload limit is slot_size - 16 bytes.
struct ChannelConfig {
  std::uint32_t slot_size = 256;    ///< Power of two, 64 to 4096.
  std::uint32_t slot_count = 4096;  ///< Power of two, 2 to 2^24.

  [[nodiscard]] constexpr core::RingGeometry geometry() const noexcept {
    return {slot_size, slot_count};
  }

  /// Largest payload a publisher may send. Narrow contract: the config is valid.
  [[nodiscard]] constexpr std::uint32_t payload_capacity() const noexcept {
    return core::payload_capacity(geometry());
  }
};

struct SubscriberConfig {
  /// How often an idle subscriber checks that a publisher still holds the lease. This bounds
  /// how long a publisher's death goes unreported; each check is one fcntl.
  std::chrono::nanoseconds liveness_probe_interval = std::chrono::milliseconds{1};

  /// How long attach() keeps retrying while a publisher is still initialising the segment.
  std::chrono::nanoseconds attach_timeout = std::chrono::seconds{1};
};

enum class LeaseMode : std::uint8_t {
  kFailIfHeld,    ///< create() throws errc::publisher_active while another publisher is live.
  kWaitForLease,  ///< create() blocks until the lease is free: a hot standby.
};

/// errc::invalid_config unless both sizes are powers of two within bounds.
[[nodiscard]] inline std::error_code validate(const ChannelConfig& config) noexcept {
  if (!core::RingGeometry::is_valid(config.slot_size, config.slot_count)) {
    return errc::invalid_config;
  }
  return {};
}

/// errc::invalid_config for negative durations; zero is valid (probe on every idle read, or
/// do not wait for initialisation).
[[nodiscard]] inline std::error_code validate(const SubscriberConfig& config) noexcept {
  if (config.liveness_probe_interval.count() < 0 || config.attach_timeout.count() < 0) {
    return errc::invalid_config;
  }
  return {};
}

}  // namespace revenant

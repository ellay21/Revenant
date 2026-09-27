#pragma once

#include <revenant/channel/channel_config.hpp>
#include <revenant/config.hpp>
#include <revenant/core/atomics.hpp>
#include <revenant/core/layout.hpp>
#include <revenant/core/ring.hpp>
#include <revenant/core/seqlock.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace revenant {

/// The single writer of a channel. It holds the channel's lease for its whole lifetime, so at
/// most one Publisher per channel is live across all processes (INV1). Subscribers can never
/// slow it down: publish() never blocks, never allocates and makes no syscalls.
///
/// Destroying a Publisher releases the lease but keeps the channel, so a restarted or standby
/// publisher resumes the sequence. Move-only; use from one thread at a time.
class Publisher {
 public:
  /// Opens or creates `channel`, takes its lease, and recovers the state a previous publisher
  /// left behind. Throws std::system_error: invalid_name, invalid_config, publisher_active
  /// (kFailIfHeld only), geometry_mismatch, or a validation error for a foreign segment.
  [[nodiscard]] static Publisher create(std::string_view channel, const ChannelConfig& config,
                                        LeaseMode mode = LeaseMode::kFailIfHeld);

  /// Publishes `payload` and returns its sequence number.
  /// Narrow contract: payload.size() <= payload_capacity().
  std::uint64_t publish(std::span<const std::byte> payload) noexcept {
    REVENANT_ASSERT(payload.size() <= capacity_);
    std::byte* const base = segment_.bytes().data();
    const std::uint64_t seq = next_seq_;
    core::write_slot(core::slot_at(base, geometry_, seq), capacity_, seq, epoch_, payload);
    core::atomics::store_release(core::control_of(base).head, seq);  // W5
    next_seq_ = seq + 1;
    return seq;
  }

  /// This publisher's epoch: one more than the previous publisher's.
  [[nodiscard]] std::uint32_t epoch() const noexcept { return epoch_; }

  /// Sequence of the last published message on this channel (by any epoch); 0 if none.
  [[nodiscard]] std::uint64_t last_sequence() const noexcept { return next_seq_ - 1; }

  [[nodiscard]] std::uint32_t payload_capacity() const noexcept { return capacity_; }
  [[nodiscard]] core::RingGeometry geometry() const noexcept { return geometry_; }

 private:
  Publisher(platform::ShmSegment segment, core::RingGeometry geometry, std::uint32_t epoch,
            std::uint64_t next_seq) noexcept;

  platform::ShmSegment segment_;
  core::RingGeometry geometry_;
  std::uint32_t capacity_;
  std::uint32_t epoch_;
  std::uint64_t next_seq_;
};

/// Removes `channel` so new attaches fail with channel_not_found; existing mappings stay valid.
/// Removing a missing channel is not an error.
void remove_channel(std::string_view channel);

}  // namespace revenant

#pragma once

#include <revenant/channel/channel_config.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace revenant {

/// A snapshot of a channel, taken without attaching, publishing or taking the lease.
struct ChannelInfo {
  ChannelConfig config;
  std::uint32_t epoch = 0;      ///< Epoch of the most recent publisher; 0 if none ever ran.
  std::uint64_t head = 0;       ///< Last fully published sequence.
  bool publisher_live = false;  ///< Whether any process holds the publisher lease right now.
  std::uint64_t created_unix_ns = 0;
};

/// Validates and reads `channel` read-only. Throws std::system_error like Subscriber::attach,
/// but never waits: an incomplete segment is segment_incomplete at once.
[[nodiscard]] ChannelInfo inspect(std::string_view channel);

/// Names of every Revenant channel on this host, sorted.
[[nodiscard]] std::vector<std::string> list_channels();

}  // namespace revenant

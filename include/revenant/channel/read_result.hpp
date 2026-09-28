#pragma once

#include <cstdint>

namespace revenant {

enum class ReadStatus : std::uint8_t {
  kMessage,  ///< The buffer holds message `first` (`length` bytes, published under `epoch`).
  kEmpty,    ///< Nothing new yet.
  kGap,      ///< Messages `first`..`last` (inclusive) were overwritten before they were read.
};

/// One outcome of Subscriber::try_read. The caller's buffer is meaningful only for kMessage.
struct [[nodiscard]] ReadResult {
  ReadStatus status{};
  std::uint64_t first{};   ///< kMessage: the sequence. kGap: first missing sequence.
  std::uint64_t last{};    ///< kGap: last missing sequence. Otherwise equal to `first`.
  std::uint32_t epoch{};   ///< kMessage: the epoch the message was published under.
  std::uint32_t length{};  ///< kMessage: bytes written to the caller's buffer.
};

}  // namespace revenant

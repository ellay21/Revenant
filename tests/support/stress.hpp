#pragma once

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace revenant::testing {

/// Messages per stress run: REVENANT_STRESS_MESSAGES, default 10^6. Call before starting threads.
[[nodiscard]] inline std::uint64_t stress_message_count() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read before any thread starts.
  const char* env = std::getenv("REVENANT_STRESS_MESSAGES");
  std::uint64_t count = 1'000'000;
  if (env != nullptr) {
    const std::string_view text{env};
    std::from_chars(text.data(), text.data() + text.size(), count);
  }
  return count;
}

}  // namespace revenant::testing

// Publishes synthetic quotes on a channel at a fixed rate.
//
//   ticker_publisher [--channel=ticker] [--rate=20000] [--standby]
//
// With --standby it blocks until the active publisher goes away, then takes over the channel
// and continues its sequence under the next epoch.

#include <revenant/revenant.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>

#include "quote.hpp"

namespace {

std::string_view flag(int argc, char** argv, std::string_view name, std::string_view fallback) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg.starts_with(name) && arg.size() > name.size() && arg[name.size()] == '=') {
      return arg.substr(name.size() + 1);
    }
  }
  return fallback;
}

bool has(int argc, char** argv, std::string_view name) {
  for (int i = 1; i < argc; ++i) {
    if (std::string_view{argv[i]} == name) {
      return true;
    }
  }
  return false;
}

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

}  // namespace

int main(int argc, char** argv) {
  const std::string channel{flag(argc, argv, "--channel", "ticker")};
  const std::uint64_t rate = std::stoull(std::string{flag(argc, argv, "--rate", "20000")});
  const bool standby = has(argc, argv, "--standby");

  try {
    if (standby) {
      std::printf("publisher: standing by for \"%s\"\n", channel.c_str());
      std::fflush(stdout);
    }
    revenant::Publisher publisher = revenant::Publisher::create(
        channel, {.slot_size = 64, .slot_count = 65536},
        standby ? revenant::LeaseMode::kWaitForLease : revenant::LeaseMode::kFailIfHeld);
    std::printf("publisher: epoch %u on \"%s\", resuming after sequence %llu\n", publisher.epoch(),
                channel.c_str(), static_cast<unsigned long long>(publisher.last_sequence()));
    std::fflush(stdout);

    const std::uint64_t interval_ns = 1'000'000'000 / (rate == 0 ? 1 : rate);
    std::uint64_t next = now_ns();
    for (std::uint32_t i = 0;; ++i) {
      while (now_ns() < next) {
      }
      next += interval_ns;
      const std::int64_t mid = 1'000'000 + static_cast<std::int64_t>(i % 200) - 100;
      const Quote quote{.sent_ns = now_ns(),
                        .symbol_id = i % 500,
                        .bid_size = 100,
                        .bid_ticks = mid - 1,
                        .ask_ticks = mid + 1,
                        .ask_size = 200,
                        .reserved = 0};
      publisher.publish(as_bytes(quote));
    }
  } catch (const std::system_error& e) {
    std::fprintf(stderr, "publisher: %s\n", e.what());
    return 1;
  }
}

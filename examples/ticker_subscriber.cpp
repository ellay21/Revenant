// Subscribes to a ticker channel and prints every event plus a line per second.
//
//   ticker_subscriber [--channel=ticker] [--seconds=0]
//
// With --seconds=N it exits after N seconds and prints a summary line that kill_demo.sh checks.

#include <revenant/revenant.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

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

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

// Waits for a publisher to create the channel, so the demo can start processes in any order.
revenant::Subscriber attach_when_ready(const std::string& channel) {
  for (int attempt = 0;; ++attempt) {
    try {
      return revenant::Subscriber::attach(channel);
    } catch (const std::system_error& e) {
      if (e.code() != revenant::errc::channel_not_found || attempt == 100) {
        throw;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  using Clock = std::chrono::steady_clock;
  const std::string channel{flag(argc, argv, "--channel", "ticker")};
  const auto seconds = std::stoll(std::string{flag(argc, argv, "--seconds", "0")});

  try {
    revenant::Subscriber subscriber = attach_when_ready(channel);
    std::printf("subscriber: attached to \"%s\" at sequence %llu, epoch %u\n", channel.c_str(),
                static_cast<unsigned long long>(subscriber.next_sequence()), subscriber.epoch());
    std::fflush(stdout);

    std::array<std::byte, 64> buffer{};
    std::uint64_t received = 0;
    std::uint64_t gaps = 0;
    std::uint64_t epochs = 0;
    std::uint64_t deaths = 0;
    std::uint64_t duplicates = 0;
    std::uint64_t last_seq = 0;
    std::uint64_t window = 0;
    std::uint64_t worst_ns = 0;
    const Clock::time_point start = Clock::now();
    Clock::time_point tick = start + std::chrono::seconds{1};

    while (seconds == 0 || Clock::now() < start + std::chrono::seconds{seconds}) {
      const revenant::ReadResult r = subscriber.try_read(buffer);
      switch (r.status) {
        case revenant::ReadStatus::kMessage: {
          duplicates += r.first <= last_seq ? 1 : 0;
          last_seq = r.first;
          const Quote quote = quote_from(std::span{buffer}.first(r.length));
          worst_ns = std::max(worst_ns, now_ns() - quote.sent_ns);
          ++received;
          ++window;
          break;
        }
        case revenant::ReadStatus::kGap:
          gaps += r.last - r.first + 1;
          std::printf("[event] gap %llu..%llu\n", static_cast<unsigned long long>(r.first),
                      static_cast<unsigned long long>(r.last));
          break;
        case revenant::ReadStatus::kEpochChange:
          ++epochs;
          std::printf("[event] epoch %u: a new publisher took over at sequence %llu\n", r.epoch,
                      static_cast<unsigned long long>(r.first));
          break;
        case revenant::ReadStatus::kPublisherDead:
          ++deaths;
          std::printf("[event] publisher dead (waiting at sequence %llu)\n",
                      static_cast<unsigned long long>(r.first));
          break;
        case revenant::ReadStatus::kEmpty:
          break;
      }
      if (Clock::now() >= tick) {
        std::printf("rate %llu msg/s  received %llu  gaps %llu  epoch %u  worst latency %llu us\n",
                    static_cast<unsigned long long>(window),
                    static_cast<unsigned long long>(received),
                    static_cast<unsigned long long>(gaps), subscriber.epoch(),
                    static_cast<unsigned long long>(worst_ns / 1000));
        std::fflush(stdout);
        window = 0;
        worst_ns = 0;
        tick += std::chrono::seconds{1};
      }
    }
    std::printf("summary: received=%llu gaps=%llu epoch_changes=%llu deaths=%llu duplicates=%llu\n",
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(gaps),
                static_cast<unsigned long long>(epochs), static_cast<unsigned long long>(deaths),
                static_cast<unsigned long long>(duplicates));
  } catch (const std::system_error& e) {
    std::fprintf(stderr, "subscriber: %s\n", e.what());
    return 1;
  }
  return 0;
}

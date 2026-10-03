// revenant-stat: a read-only view of the channels on this host. It never attaches as a
// subscriber, never takes the lease and never writes, so it cannot disturb a live channel.
//
//   revenant-stat --list              one line per channel
//   revenant-stat <channel>           one channel
//   revenant-stat <channel> --watch   refresh every second, with the publish rate
//
// Exit codes: 0 success, 1 usage, 2 channel not found, 3 validation failed.

#include <revenant/channel/inspect.hpp>
#include <revenant/errors.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

constexpr int kUsage = 1;
constexpr int kNotFound = 2;
constexpr int kInvalid = 3;

void print(std::string_view name, const revenant::ChannelInfo& info) {
  std::printf("%-24.*s slots %8u x %4u B   epoch %5u   head %14llu   publisher: %s\n",
              static_cast<int>(name.size()), name.data(), info.config.slot_count,
              info.config.slot_size, info.epoch, static_cast<unsigned long long>(info.head),
              info.publisher_live ? "live" : "none");
}

int report(std::string_view name, const std::system_error& e) {
  std::fprintf(stderr, "revenant-stat: %.*s: %s\n", static_cast<int>(name.size()), name.data(),
               e.code().message().c_str());
  return e.code() == revenant::errc::channel_not_found ? kNotFound : kInvalid;
}

int list() {
  int status = 0;
  for (const std::string& name : revenant::list_channels()) {
    try {
      print(name, revenant::inspect(name));
    } catch (const std::system_error& e) {
      status = report(name, e);
    }
  }
  return status;
}

[[noreturn]] void watch(std::string_view name) {
  using Clock = std::chrono::steady_clock;
  revenant::ChannelInfo previous = revenant::inspect(name);
  Clock::time_point then = Clock::now();
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds{1});
    const revenant::ChannelInfo now = revenant::inspect(name);
    const Clock::time_point when = Clock::now();
    const double seconds = std::chrono::duration<double>(when - then).count();
    const auto published = static_cast<double>(now.head - previous.head);
    std::printf("epoch %u  head %llu  rate %.0f msg/s  publisher: %s\n", now.epoch,
                static_cast<unsigned long long>(now.head), published / seconds,
                now.publisher_live ? "live" : "none");
    std::fflush(stdout);
    previous = now;
    then = when;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view first = argc > 1 ? argv[1] : "";
  if (argc == 2 && first == "--list") {
    return list();
  }
  if (argc < 2 || argc > 3 || first.starts_with("-") ||
      (argc == 3 && std::string_view{argv[2]} != "--watch")) {
    std::fprintf(stderr, "usage: revenant-stat --list | <channel> [--watch]\n");
    return kUsage;
  }
  try {
    print(first, revenant::inspect(first));
    if (argc == 3) {
      watch(first);
    }
  } catch (const std::system_error& e) {
    return report(first, e);
  }
  return 0;
}

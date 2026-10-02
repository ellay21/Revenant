#pragma once

#include <sched.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Small, dependency-free benchmark harness: exact percentiles over raw samples, CPU pinning and
// environment disclosure. Numbers are reported as distributions, never as means.

namespace revenant::bench {

[[nodiscard]] inline std::uint64_t now_ns() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

/// Pins the calling process to `cpu` modulo the online CPU count; false if the kernel refused.
inline bool pin_to_cpu(int cpu) noexcept {
  const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpu % std::max(1L, online)), &set);
  return ::sched_setaffinity(0, sizeof set, &set) == 0;
}

/// A short or failed transfer means the benchmark itself is broken, so it stops immediately.
inline void transferred(ssize_t got, std::size_t want, const char* what) noexcept {
  if (got != static_cast<ssize_t>(want)) {
    std::perror(what);
    std::_Exit(1);
  }
}
inline void write_all(int fd, const void* data, std::size_t n) noexcept {
  transferred(::write(fd, data, n), n, "write");
}
inline void read_exact(int fd, void* data, std::size_t n) noexcept {
  transferred(::read(fd, data, n), n, "read");
}
inline void send_all(int fd, const void* data, std::size_t n) noexcept {
  transferred(::send(fd, data, n, 0), n, "send");
}
inline void recv_exact(int fd, void* data, std::size_t n) noexcept {
  transferred(::recv(fd, data, n, 0), n, "recv");
}

/// Raw samples in a preallocated vector; sorted once, so every percentile is exact.
class Samples {
 public:
  explicit Samples(std::size_t capacity) { values_.reserve(capacity); }
  void add(std::uint64_t ns) { values_.push_back(ns); }
  [[nodiscard]] std::size_t size() const noexcept { return values_.size(); }

  /// Nearest-rank percentile, p in [0, 100]. Sorts on first use.
  [[nodiscard]] std::uint64_t percentile(double p) {
    if (values_.empty()) {
      return 0;
    }
    if (!sorted_) {
      std::sort(values_.begin(), values_.end());
      sorted_ = true;
    }
    const auto n = static_cast<double>(values_.size());
    const auto rank = static_cast<std::size_t>(std::max(1.0, p / 100.0 * n + 0.5));
    return values_[std::min(rank, values_.size()) - 1];
  }

 private:
  std::vector<std::uint64_t> values_;
  bool sorted_ = false;
};

inline void print_header(std::string_view what) {
  std::printf("\n%s\n", std::string(what).c_str());
  std::printf("| %-34s | %9s | %9s | %9s | %9s | %9s | %9s |\n", "case (ns)", "samples", "p50",
              "p90", "p99", "p99.9", "max");
  std::printf("|%s|%s|%s|%s|%s|%s|%s|\n", std::string(36, '-').c_str(),
              std::string(11, '-').c_str(), std::string(11, '-').c_str(),
              std::string(11, '-').c_str(), std::string(11, '-').c_str(),
              std::string(11, '-').c_str(), std::string(11, '-').c_str());
}

inline void print_row(std::string_view name, Samples& s) {
  std::printf("| %-34s | %9zu | %9llu | %9llu | %9llu | %9llu | %9llu |\n",
              std::string(name).c_str(), s.size(),
              static_cast<unsigned long long>(s.percentile(50)),
              static_cast<unsigned long long>(s.percentile(90)),
              static_cast<unsigned long long>(s.percentile(99)),
              static_cast<unsigned long long>(s.percentile(99.9)),
              static_cast<unsigned long long>(s.percentile(100)));
}

/// `--name=value` for an unsigned option, or `fallback`.
[[nodiscard]] inline std::uint64_t option(int argc, char** argv, std::string_view name,
                                          std::uint64_t fallback) {
  const std::string prefix = "--" + std::string(name) + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg.starts_with(prefix)) {
      std::uint64_t value = fallback;
      std::from_chars(arg.data() + prefix.size(), arg.data() + arg.size(), value);
      return value;
    }
  }
  return fallback;
}

/// CPU model, online CPUs and kernel, so every published number states where it came from.
inline void print_environment() {
  std::string model = "unknown CPU";
  std::ifstream cpuinfo{"/proc/cpuinfo"};
  for (std::string line; std::getline(cpuinfo, line);) {
    if (line.starts_with("model name")) {
      model = line.substr(line.find(':') + 2);
      break;
    }
  }
  utsname host{};
  ::uname(&host);
  std::printf("environment: %s, %ld online CPUs, Linux %s\n", model.c_str(),
              ::sysconf(_SC_NPROCESSORS_ONLN), host.release);
}

}  // namespace revenant::bench

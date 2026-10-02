#include <revenant/revenant.hpp>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "bench_util.hpp"

// Fan-out: one publisher, N subscriber processes, messages paced at a fixed rate. Revenant writes
// each message once whatever N is; a Unix-socket publisher must send it N times, and a slow
// reader pushes back on it. Reported per N: the publisher's cost per message, and each
// subscriber's delivery latency (publish timestamp to receipt, CLOCK_MONOTONIC across processes).

namespace {

namespace bench = revenant::bench;
constexpr std::size_t kMessage = 64;
constexpr revenant::ChannelConfig kConfig{.slot_size = 128, .slot_count = 4096};

struct Report {
  std::uint64_t p50 = 0;
  std::uint64_t p99 = 0;
  std::uint64_t p999 = 0;
  std::uint64_t gaps = 0;
};

struct Message {
  std::uint64_t seq;
  std::uint64_t sent_ns;
};

void write_report(int fd, bench::Samples& latency, std::uint64_t gaps) {
  const Report report{latency.percentile(50), latency.percentile(99), latency.percentile(99.9),
                      gaps};
  bench::write_all(fd, &report, sizeof report);
}

void throw_errno(const char* what) {
  throw std::system_error(errno, std::system_category(), what);
}

// Forks `n` readers running `body(index, report_fd)`; returns their report pipes.
template <typename Body>
std::vector<int> spawn_readers(std::size_t n, Body body) {
  std::vector<int> reports;
  for (std::size_t i = 0; i < n; ++i) {
    int fds[2];
    if (::pipe(fds) != 0) {
      throw_errno("pipe");
    }
    if (::fork() == 0) {
      ::close(fds[0]);
      bench::pin_to_cpu(static_cast<int>(i) + 1);
      body(i, fds[1]);
      std::_Exit(0);
    }
    ::close(fds[1]);
    reports.push_back(fds[0]);
  }
  return reports;
}

// The worst subscriber's report, so the row shows what the slowest reader experienced.
Report collect(const std::vector<int>& reports) {
  Report worst;
  for (const int fd : reports) {
    Report r;
    if (::read(fd, &r, sizeof r) == static_cast<ssize_t>(sizeof r)) {
      worst.p50 = std::max(worst.p50, r.p50);
      worst.p99 = std::max(worst.p99, r.p99);
      worst.p999 = std::max(worst.p999, r.p999);
      worst.gaps = std::max(worst.gaps, r.gaps);
    }
    ::close(fd);
  }
  while (::wait(nullptr) > 0) {
  }
  return worst;
}

template <typename Send>
void paced(std::uint64_t messages, std::uint64_t interval_ns, bench::Samples& cost, Send send) {
  const std::uint64_t start = bench::now_ns() + 1'000'000;
  for (std::uint64_t seq = 1; seq <= messages; ++seq) {
    while (bench::now_ns() < start + seq * interval_ns) {
    }
    const std::uint64_t t0 = bench::now_ns();
    send(Message{seq, t0});
    cost.add(bench::now_ns() - t0);
  }
}

Report run_revenant(std::size_t n, std::uint64_t messages, std::uint64_t interval_ns,
                    bench::Samples& cost) {
  const std::string channel = "bench_fanout_" + std::to_string(::getpid());
  revenant::Publisher publisher = revenant::Publisher::create(channel, kConfig);
  int ready[2];
  if (::pipe(ready) != 0) {
    throw_errno("pipe");
  }
  const std::vector<int> reports = spawn_readers(n, [&](std::size_t, int report_fd) {
    revenant::Subscriber subscriber = revenant::Subscriber::attach(channel);
    const char go = 1;
    bench::write_all(ready[1], &go, 1);
    std::array<std::byte, kConfig.payload_capacity()> buffer{};
    bench::Samples latency{messages};
    std::uint64_t gaps = 0;
    for (;;) {
      const revenant::ReadResult r = subscriber.try_read(buffer);
      if (r.status == revenant::ReadStatus::kMessage) {
        Message m;
        std::memcpy(&m, buffer.data(), sizeof m);
        latency.add(bench::now_ns() - m.sent_ns);
      } else if (r.status == revenant::ReadStatus::kGap) {
        gaps += r.last - r.first + 1;
      }
      if ((r.status == revenant::ReadStatus::kMessage || r.status == revenant::ReadStatus::kGap) &&
          r.last >= messages) {
        break;
      }
    }
    write_report(report_fd, latency, gaps);
  });
  for (std::size_t i = 0; i < n; ++i) {
    char go = 0;
    bench::read_exact(ready[0], &go, 1);
  }
  ::close(ready[0]);
  ::close(ready[1]);

  bench::pin_to_cpu(0);
  std::array<std::byte, kMessage> payload{};
  paced(messages, interval_ns, cost, [&](const Message& m) {
    std::memcpy(payload.data(), &m, sizeof m);
    publisher.publish(payload);
  });
  const Report report = collect(reports);
  revenant::remove_channel(channel);
  return report;
}

Report run_unix_sockets(std::size_t n, std::uint64_t messages, std::uint64_t interval_ns,
                        bench::Samples& cost) {
  std::vector<int> publisher_ends;
  std::vector<int> reader_ends;
  for (std::size_t i = 0; i < n; ++i) {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) != 0) {
      throw_errno("socketpair");
    }
    publisher_ends.push_back(fds[0]);
    reader_ends.push_back(fds[1]);
  }
  const std::vector<int> reports = spawn_readers(n, [&](std::size_t index, int report_fd) {
    bench::Samples latency{messages};
    Message m{};
    while (m.seq < messages && ::recv(reader_ends[index], &m, sizeof m, 0) > 0) {
      latency.add(bench::now_ns() - m.sent_ns);
    }
    write_report(report_fd, latency, 0);
  });
  for (const int fd : reader_ends) {
    ::close(fd);
  }

  bench::pin_to_cpu(0);
  std::array<std::byte, kMessage> payload{};
  paced(messages, interval_ns, cost, [&](const Message& m) {
    std::memcpy(payload.data(), &m, sizeof m);
    for (const int fd : publisher_ends) {
      bench::send_all(fd, payload.data(), payload.size());
    }
  });
  const Report report = collect(reports);
  for (const int fd : publisher_ends) {
    ::close(fd);
  }
  return report;
}

void print(const char* transport, std::size_t n, bench::Samples& cost, const Report& r) {
  std::printf("| %-12s | %2zu | %9llu | %9llu | %11llu | %11llu | %11llu | %6llu |\n", transport, n,
              static_cast<unsigned long long>(cost.percentile(50)),
              static_cast<unsigned long long>(cost.percentile(99)),
              static_cast<unsigned long long>(r.p50), static_cast<unsigned long long>(r.p99),
              static_cast<unsigned long long>(r.p999), static_cast<unsigned long long>(r.gaps));
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t messages = bench::option(argc, argv, "messages", 200'000);
  const std::uint64_t interval_ns = bench::option(argc, argv, "interval-ns", 5'000);
  const std::uint64_t max_subscribers = bench::option(argc, argv, "max-subscribers", 8);
  bench::print_environment();
  std::printf("\nfan-out, 64-byte messages paced every %llu ns, %llu messages per run\n",
              static_cast<unsigned long long>(interval_ns),
              static_cast<unsigned long long>(messages));
  std::printf(
      "| transport    |  N | pub p50   | pub p99   | deliver p50 | deliver p99 | deliver p99.9 "
      "| gaps   |\n");
  std::printf(
      "|--------------|----|-----------|-----------|-------------|-------------|------------"
      "---|--------|\n");
  for (std::size_t n = 1; n <= max_subscribers; n *= 2) {
    bench::Samples revenant_cost{messages};
    const Report revenant_report = run_revenant(n, messages, interval_ns, revenant_cost);
    print("revenant", n, revenant_cost, revenant_report);
    bench::Samples socket_cost{messages};
    const Report socket_report = run_unix_sockets(n, messages, interval_ns, socket_cost);
    print("unix socket", n, socket_cost, socket_report);
  }
  std::printf("(all times in ns; delivery columns show the slowest subscriber)\n");
  return 0;
}

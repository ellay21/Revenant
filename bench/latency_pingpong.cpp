#include <revenant/revenant.hpp>

#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <system_error>

#include "bench_util.hpp"

// One-way latency between two pinned processes, measured as round-trip time / 2 over a closed
// loop: the next ping is sent only after the previous pong arrives, so there is no queueing and
// no coordinated omission. Revenant uses two channels (ping, pong); the baseline is the same
// exchange over socketpair(AF_UNIX, SOCK_SEQPACKET). Messages are 64 bytes.

namespace {

namespace bench = revenant::bench;
constexpr std::size_t kMessage = 64;
constexpr revenant::ChannelConfig kConfig{.slot_size = 128, .slot_count = 1024};

using Buffer = std::array<std::byte, kConfig.payload_capacity()>;

// Spins until the next message; any other event in a private ping-pong is a bug.
std::uint64_t receive(revenant::Subscriber& subscriber, Buffer& buffer) {
  for (;;) {
    const revenant::ReadResult r = subscriber.try_read(buffer);
    if (r.status == revenant::ReadStatus::kMessage) {
      return r.first;
    }
    if (r.status != revenant::ReadStatus::kEmpty) {
      std::fprintf(stderr, "unexpected read status %d\n", static_cast<int>(r.status));
      std::_Exit(1);
    }
  }
}

void bench_revenant(std::uint64_t warmup, std::uint64_t iterations, bench::Samples& samples) {
  const std::string ping = "bench_ping_" + std::to_string(::getpid());
  const std::string pong = "bench_pong_" + std::to_string(::getpid());
  revenant::Publisher ping_out = revenant::Publisher::create(ping, kConfig);

  int ready[2];
  if (::pipe(ready) != 0) {
    throw std::system_error(errno, std::system_category(), "pipe");
  }
  const pid_t child = ::fork();
  if (child == 0) {
    bench::pin_to_cpu(1);
    revenant::Subscriber ping_in = revenant::Subscriber::attach(ping);
    revenant::Publisher pong_out = revenant::Publisher::create(pong, kConfig);
    const char go = 1;
    bench::write_all(ready[1], &go, 1);
    Buffer buffer{};
    for (std::uint64_t i = 0; i < warmup + iterations; ++i) {
      static_cast<void>(receive(ping_in, buffer));
      pong_out.publish(std::span{buffer}.first(kMessage));
    }
    std::_Exit(0);
  }
  bench::pin_to_cpu(0);
  char go = 0;
  bench::read_exact(ready[0], &go, 1);
  revenant::Subscriber pong_in = revenant::Subscriber::attach(pong);

  Buffer buffer{};
  for (std::uint64_t i = 0; i < warmup + iterations; ++i) {
    const std::uint64_t start = bench::now_ns();
    ping_out.publish(std::span{buffer}.first(kMessage));
    static_cast<void>(receive(pong_in, buffer));
    if (i >= warmup) {
      samples.add((bench::now_ns() - start) / 2);
    }
  }
  ::waitpid(child, nullptr, 0);
  ::close(ready[0]);
  ::close(ready[1]);
  revenant::remove_channel(ping);
  revenant::remove_channel(pong);
}

void bench_unix_socket(std::uint64_t warmup, std::uint64_t iterations, bench::Samples& samples) {
  int fds[2];
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds) != 0) {
    throw std::system_error(errno, std::system_category(), "socketpair");
  }
  std::array<std::byte, kMessage> buffer{};
  const pid_t child = ::fork();
  if (child == 0) {
    bench::pin_to_cpu(1);
    ::close(fds[0]);
    for (std::uint64_t i = 0; i < warmup + iterations; ++i) {
      bench::recv_exact(fds[1], buffer.data(), buffer.size());
      bench::send_all(fds[1], buffer.data(), buffer.size());
    }
    std::_Exit(0);
  }
  bench::pin_to_cpu(0);
  ::close(fds[1]);
  for (std::uint64_t i = 0; i < warmup + iterations; ++i) {
    const std::uint64_t start = bench::now_ns();
    bench::send_all(fds[0], buffer.data(), buffer.size());
    bench::recv_exact(fds[0], buffer.data(), buffer.size());
    if (i >= warmup) {
      samples.add((bench::now_ns() - start) / 2);
    }
  }
  ::waitpid(child, nullptr, 0);
  ::close(fds[0]);
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t iterations = bench::option(argc, argv, "iterations", 1'000'000);
  const std::uint64_t warmup = bench::option(argc, argv, "warmup", iterations / 10);
  bench::print_environment();

  bench::Samples revenant_samples{iterations};
  bench::Samples socket_samples{iterations};
  bench_revenant(warmup, iterations, revenant_samples);
  bench_unix_socket(warmup, iterations, socket_samples);

  bench::print_header("one-way latency, 64-byte messages, cross-process ping-pong (RTT / 2)");
  bench::print_row("revenant (shared memory)", revenant_samples);
  bench::print_row("unix socket (SOCK_SEQPACKET)", socket_samples);
  return 0;
}

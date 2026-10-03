#include <revenant/revenant.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <system_error>
#include <thread>

#include "bench_util.hpp"

// The two numbers behind the crash-safety claims, measured with real SIGKILLs:
//   death detection: SIGKILL sent -> an idle subscriber returns kPublisherDead
//   takeover:        SIGKILL sent -> the subscriber receives the first message of the next epoch,
//                    published by a hot standby blocked in Publisher::create(kWaitForLease)
// Both include the kernel tearing the process down, which is what a real crash costs.

namespace {

namespace bench = revenant::bench;
using namespace std::chrono_literals;
constexpr revenant::ChannelConfig kConfig{.slot_size = 128, .slot_count = 1024};
using Buffer = std::array<std::byte, kConfig.payload_capacity()>;

// Forks a publisher that publishes one message and then waits to be killed. With `standby`, it
// first reports that it is about to block on the lease.
pid_t spawn_publisher(const std::string& channel, bool standby, int report_fd) {
  const pid_t pid = ::fork();
  if (pid != 0) {
    return pid;
  }
  if (standby) {
    const char waiting = 1;
    bench::write_all(report_fd, &waiting, 1);
  }
  revenant::Publisher publisher = revenant::Publisher::create(
      channel, kConfig,
      standby ? revenant::LeaseMode::kWaitForLease : revenant::LeaseMode::kFailIfHeld);
  const std::array<std::byte, 8> payload{};
  publisher.publish(payload);
  if (!standby) {
    const char ready = 1;
    bench::write_all(report_fd, &ready, 1);
  }
  for (;;) {
    ::pause();
  }
}

// Reads until the subscriber has caught up with the current publisher's message.
void catch_up(revenant::Subscriber& subscriber, Buffer& buffer, std::uint32_t epoch) {
  for (;;) {
    const revenant::ReadResult r = subscriber.try_read(buffer);
    if (r.status == revenant::ReadStatus::kMessage && r.epoch == epoch) {
      return;
    }
  }
}

void kill_and_reap(pid_t pid) {
  ::kill(pid, SIGKILL);
  ::waitpid(pid, nullptr, 0);
}

void measure_detection(const std::string& channel, std::uint64_t iterations,
                       std::chrono::nanoseconds probe, bench::Samples& samples) {
  int report[2];
  if (::pipe(report) != 0) {
    throw std::system_error(errno, std::system_category(), "pipe");
  }
  pid_t publisher = spawn_publisher(channel, false, report[1]);
  char byte = 0;
  bench::read_exact(report[0], &byte, 1);
  revenant::Subscriber subscriber =
      revenant::Subscriber::attach(channel, {.liveness_probe_interval = probe});
  Buffer buffer{};

  for (std::uint64_t i = 0; i < iterations; ++i) {
    const std::uint64_t start = bench::now_ns();
    ::kill(publisher, SIGKILL);
    while (subscriber.try_read(buffer).status != revenant::ReadStatus::kPublisherDead) {
    }
    samples.add(bench::now_ns() - start);
    ::waitpid(publisher, nullptr, 0);

    publisher = spawn_publisher(channel, false, report[1]);
    bench::read_exact(report[0], &byte, 1);
    catch_up(subscriber, buffer, static_cast<std::uint32_t>(i) + 2);
  }
  kill_and_reap(publisher);
  ::close(report[0]);
  ::close(report[1]);
}

void measure_takeover(const std::string& channel, std::uint64_t iterations,
                      bench::Samples& samples) {
  int report[2];
  if (::pipe(report) != 0) {
    throw std::system_error(errno, std::system_category(), "pipe");
  }
  pid_t active = spawn_publisher(channel, false, report[1]);
  char byte = 0;
  bench::read_exact(report[0], &byte, 1);
  revenant::Subscriber subscriber = revenant::Subscriber::attach(channel);
  Buffer buffer{};

  for (std::uint64_t i = 0; i < iterations; ++i) {
    const pid_t standby = spawn_publisher(channel, true, report[1]);
    bench::read_exact(report[0], &byte, 1);
    std::this_thread::sleep_for(2ms);  // let the standby reach F_OFD_SETLKW
    const std::uint32_t next_epoch = subscriber.epoch() + 1;

    const std::uint64_t start = bench::now_ns();
    ::kill(active, SIGKILL);
    catch_up(subscriber, buffer, next_epoch);
    samples.add(bench::now_ns() - start);
    ::waitpid(active, nullptr, 0);
    active = standby;
  }
  kill_and_reap(active);
  ::close(report[0]);
  ::close(report[1]);
}

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t iterations = bench::option(argc, argv, "iterations", 1'000);
  const std::string channel = "bench_recovery_" + std::to_string(::getpid());
  bench::print_environment();

  bench::Samples detection_1ms{iterations};
  bench::Samples detection_100us{iterations};
  bench::Samples takeover{iterations};
  measure_detection(channel, iterations, 1ms, detection_1ms);
  revenant::remove_channel(channel);
  measure_detection(channel, iterations, 100us, detection_100us);
  revenant::remove_channel(channel);
  measure_takeover(channel, iterations, takeover);
  revenant::remove_channel(channel);

  bench::print_header("recovery after SIGKILL of the publisher");
  bench::print_row("death detected (probe every 1 ms)", detection_1ms);
  bench::print_row("death detected (probe every 100 us)", detection_100us);
  bench::print_row("hot-standby takeover, first message", takeover);
  return 0;
}

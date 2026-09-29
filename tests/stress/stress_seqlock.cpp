#include <revenant/core/layout.hpp>
#include <revenant/core/seqlock.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "support/payload.hpp"
#include "support/stress.hpp"

// INV2, INV4 and INV5 under real races: one writer, three readers and an observer share an
// eight-slot ring, so readers are lapped and torn constantly. Every delivered message is
// self-validating, so a single wrong byte fails the test.

namespace {

namespace core = revenant::core;
namespace testing = revenant::testing;
using core::SlotState;

constexpr core::RingGeometry kGeometry{64, 8};
constexpr std::uint32_t kCapacity = core::payload_capacity(kGeometry);
constexpr std::uint32_t kEpoch = 1;
constexpr std::size_t kReaders = 3;

struct ReaderStats {
  std::uint64_t received = 0;
  std::uint64_t gaps = 0;
  std::uint64_t torn = 0;
  std::uint64_t lapped = 0;
  std::uint64_t next = 1;
  std::string failure;
};

// The subscriber checker: every result must be consistent with an exact prefix of the stream.
template <typename Hook>
void run_reader(const std::byte* segment, std::uint64_t messages, ReaderStats& stats, Hook hook) {
  std::array<std::byte, kCapacity> out{};
  std::uint64_t waits = 0;
  while (stats.next <= messages) {
    const std::byte* slot = core::slot_at(segment, kGeometry, stats.next);
    const core::SlotRead r = core::read_slot(slot, kCapacity, stats.next, out, hook);
    switch (r.state) {
      case SlotState::kCommitted:
        if (r.epoch != kEpoch || !testing::check_payload(stats.next, kEpoch, kCapacity,
                                                         std::span{out}.first(r.length))) {
          stats.failure = "INV4: corrupt message " + std::to_string(stats.next);
          return;
        }
        ++stats.received;
        ++stats.next;
        break;
      case SlotState::kNotYet:
      case SlotState::kInProgress:
        if (++waits % 64 == 0) {
          std::this_thread::yield();
        }
        break;
      case SlotState::kLapped:
      case SlotState::kTorn: {
        ++(r.state == SlotState::kTorn ? stats.torn : stats.lapped);
        constexpr std::uint64_t kHalfRing = kGeometry.slot_count / 2;
        const std::uint64_t behind = r.observed_seq > kHalfRing ? r.observed_seq - kHalfRing : 1;
        const std::uint64_t resume = std::max(stats.next + 1, behind);
        stats.gaps += resume - stats.next;
        stats.next = resume;
        break;
      }
      case SlotState::kCorrupt:
        stats.failure = "INV7: corrupt length at " + std::to_string(stats.next);
        return;
    }
  }
}

TEST(StressSeqlock, ConcurrentReadersNeverDeliverATornOrMisorderedMessage) {
  const std::uint64_t messages = testing::stress_message_count();
  std::vector<std::byte> segment(core::segment_size(kGeometry));
  core::initialize_segment(segment, kGeometry, 0);
  const std::byte* read_only = segment.data();

  std::latch start{static_cast<std::ptrdiff_t>(kReaders) + 2};
  std::atomic<bool> stop_observer{false};
  std::array<ReaderStats, kReaders> readers{};
  std::string observer_failure;

  std::thread writer{[&] {
    std::array<std::byte, kCapacity> payload{};
    start.arrive_and_wait();
    for (std::uint64_t seq = 1; seq <= messages; ++seq) {
      const std::size_t length = testing::make_payload(seq, kEpoch, kCapacity, payload);
      core::write_slot(core::slot_at(segment.data(), kGeometry, seq), kCapacity, seq, kEpoch,
                       std::span{payload}.first(length));
    }
  }};

  std::vector<std::thread> reader_threads;
  for (std::size_t i = 0; i < kReaders; ++i) {
    reader_threads.emplace_back([&, i] {
      start.arrive_and_wait();
      if (i == 0) {
        // Yielding between copy and re-check widens the race window, so torn reads must occur.
        std::uint64_t calls = 0;
        run_reader(read_only, messages, readers[i], [&calls] {
          if (++calls % 8 == 0) {
            std::this_thread::yield();
          }
        });
      } else {
        run_reader(read_only, messages, readers[i], core::NoHook{});
      }
    });
  }

  // INV2: no slot's word ever decreases.
  std::thread observer{[&] {
    std::array<std::uint64_t, kGeometry.slot_count> last{};
    start.arrive_and_wait();
    while (!stop_observer.load()) {
      for (std::uint32_t i = 0; i < kGeometry.slot_count; ++i) {
        const std::uint64_t word = core::load_seq_word(core::slot_at(read_only, kGeometry, i));
        if (word < last[i]) {
          observer_failure = "INV2: slot " + std::to_string(i) + " went backwards";
          return;
        }
        last[i] = word;
      }
    }
  }};

  writer.join();
  for (std::thread& t : reader_threads) {
    t.join();
  }
  stop_observer.store(true);
  observer.join();

  EXPECT_EQ(observer_failure, "");
  std::uint64_t torn = 0;
  for (const ReaderStats& stats : readers) {
    EXPECT_EQ(stats.failure, "");
    // INV5: every sequence was either delivered once, in order, or counted in a gap.
    EXPECT_EQ(stats.next, messages + 1);
    EXPECT_EQ(stats.received + stats.gaps, messages);
    EXPECT_GT(stats.received, 0U);
    torn += stats.torn;
  }
  EXPECT_GT(torn, 0U) << "no torn read happened, so the race was never exercised";
  RecordProperty("messages", std::to_string(messages));
  RecordProperty("torn_reads", std::to_string(torn));
}

}  // namespace

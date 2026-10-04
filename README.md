# Revenant

> Crash-safe shared-memory market-data fan-out for C++20 on Linux.

[![ci](https://github.com/ellay21/Revenant/actions/workflows/ci.yml/badge.svg)](https://github.com/ellay21/Revenant/actions/workflows/ci.yml)

One publisher process, typically a feed handler, broadcasts small messages (quotes, trades, book updates) to any number of subscriber processes on the same host. It uses a lock-free ring in shared memory, so each message is written **once**, whatever the number of subscribers, and no subscriber can ever slow the publisher down.

The hard part is not speed; it is what happens when a process dies halfway through an operation. Most shared-memory rings leave that undefined. In practice that means torn reads, wedged readers, or a "dead" publisher that nobody notices. Revenant defines each case and proves it with real `SIGKILL`s:

| | Guarantee | Mechanism |
|---|---|---|
| **G1** | **No torn reads.** A subscriber never delivers a message that was half-written, or overwritten while it was being read. | A seqlock per slot: copy out with relaxed atomic words, then re-validate |
| **G2** | **No silent loss.** A subscriber that falls behind gets an exact `Gap{first, last}`, then continues. | Monotonic 64-bit sequence numbers, encoded in each slot's seqlock word |
| **G3** | **Bounded death detection.** When the publisher dies, idle subscribers learn about it within a configurable interval (1 ms by default). | The publisher holds an [open-file-description lock](docs/design.md#6-the-publisher-lease) on the segment, which the kernel releases on death. Subscribers can query it without taking it. |
| **G4** | **Single writer, seamless takeover.** At most one publisher is live. A restarted or hot-standby publisher resumes the sequence exactly, and subscribers get an explicit `EpochChange`. There is no duplicate and no lost committed message. | The same lease, plus O(1) recovery |

## Results

Indicative numbers from a shared 2-vCPU GitHub Codespace ([full results](bench/results/codespace-2vcpu-2026-10-03.md), [methodology](bench/results/METHODOLOGY.md)):

| Measurement | Revenant | Unix socket (`SOCK_SEQPACKET`) |
|---|---:|---:|
| One-way latency, cross-process ping-pong, p50 / p99 | **95 ns / 130 ns** | 6.9 µs / 14.6 µs |
| Publisher cost per message, 1 → 2 → 4 subscribers (p50) | **50 → 40 → 40 ns** (flat) | 2.2 → 4.4 → 11.2 µs (linear) |
| Publisher `SIGKILL` → subscriber told, 1 ms / 100 µs probe (p50) | **0.81 ms / 0.11 ms** | — |
| Publisher `SIGKILL` → first message from hot standby (p50) | **0.14 ms** | — |

## Quick start

```bash
cmake --preset release && cmake --build --preset release
examples/kill_demo.sh build/release
```

The demo starts a publisher, a hot standby and a subscriber, then sends the publisher `kill -9`:

```text
>>> kill -9 the publisher (pid 102397)
publisher: epoch 2 on "kill_demo_102395", resuming after sequence 40222
[event] epoch 2: a new publisher took over at sequence 40223
summary: received=79988 gaps=0 epoch_changes=1 deaths=0 duplicates=0
OK: the standby took over and the stream continued with no gap and no duplicate
```

CI runs this demo on every push.

## Usage

```cpp
#include <revenant/revenant.hpp>

// Publisher process (or a hot standby: LeaseMode::kWaitForLease).
auto pub = revenant::Publisher::create("quotes", {.slot_size = 128, .slot_count = 65536});
pub.publish(std::as_bytes(std::span{&quote, 1}));      // never blocks, no syscalls

// Any number of subscriber processes.
auto sub = revenant::Subscriber::attach("quotes");
std::array<std::byte, 128> buf;
for (;;) {
  const revenant::ReadResult r = sub.try_read(buf);     // never blocks
  switch (r.status) {
    case revenant::ReadStatus::kMessage:       handle(std::span{buf}.first(r.length)); break;
    case revenant::ReadStatus::kGap:           request_snapshot(r.first, r.last);      break;
    case revenant::ReadStatus::kEpochChange:   on_publisher_takeover(r.epoch);         break;
    case revenant::ReadStatus::kPublisherDead: alert();                                break;
    case revenant::ReadStatus::kEmpty:                                                 break;
  }
}
```

`revenant-stat --list` shows every channel on the host, its geometry, epoch, head and whether a publisher is live. It inspects without attaching, and can never disturb a channel.

## How it is verified

| Suite | What it proves |
|---|---|
| **unit** (81 tests) | Ring arithmetic, the byte-exact wire layout, header validation against 10⁵ seeded hostile mutations, and the seqlock read and write protocol, including torn reads forced deterministically through a test seam |
| **integration** (70) | The real `/dev/shm`, OFD-lock semantics (including zombie and `fork` cases), publisher creation and recovery, subscriber gaps, epochs and death detection |
| **stress** (2) | Threads racing through tiny rings over real shared memory. Every delivered byte is self-validating, and the run must observe torn reads. |
| **crash** (29) | A forked publisher `SIGKILL`ed at each of seven fault points in the write and recovery protocol, at several points in the ring. Plus hot-standby takeover with exactly one winner per death. |

- **CI:** GCC 13, GCC 14 and Clang 18 in Debug and Release, on x86-64 and **arm64** (whose weaker memory model exposes ordering bugs that x86 hides), plus AddressSanitizer + UBSan, ThreadSanitizer, clang-tidy and clang-format.
- **Mutation checks:** removing the seqlock re-check, the INV3 roll-forward, or the OFD lock semantics makes specific tests fail. That shows the tests catch the bugs they exist for.

## Design

- [docs/design.md](docs/design.md) covers why fixed slots and seqlocks, why each memory ordering is sufficient, why reads copy out instead of being zero-copy, the publisher lease, recovery, and the full failure-semantics table.
- [docs/wire-format.md](docs/wire-format.md) is the byte-exact shared-memory layout (v1). Every offset is pinned by `static_assert`s and tests.

Every atomic access goes through one header, [`core/atomics.hpp`](include/revenant/core/atomics.hpp), whose functions name their memory ordering. Lint fails if `std::memory_order` appears anywhere else.

## Scope

Revenant does one thing: a single-host, single-publisher, lossy broadcast with defined crash semantics.

**Non-goals:** multiple publishers per channel, cross-host transport, persistence or replay, back-pressure, zero-copy reads, and detecting a *stalled* (rather than dead) publisher. Messages must fit in one slot, at most 4080 bytes.

## Building and testing

Requirements: Linux 3.15+ (for OFD locks), a C++20 compiler (GCC 12+ or Clang 16+), CMake 3.25+ and Ninja. GoogleTest is downloaded at configure time, and only when tests are built.

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev   # also: release, asan, tsan
cmake --preset bench && cmake --build --preset bench                     # benchmarks
```

The [dev container](.devcontainer/devcontainer.json) gives you the same Ubuntu 24.04 toolchain that CI uses.

## License

Apache-2.0

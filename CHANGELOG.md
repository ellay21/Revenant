# Changelog

All notable changes to this project are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html). The wire format is versioned separately: see [docs/wire-format.md](docs/wire-format.md).

## [0.1.0] — 2026-10-05

First release.

**Wire format: version 1.**

### Added

- **`Publisher`**: the single writer of a channel.
  - It holds a kernel-arbitrated OFD-lock lease, so the kernel enforces one live publisher per channel.
  - `publish` never blocks, allocates or makes a syscall.
  - Start-up recovers any state a dead publisher left behind, including a segment abandoned mid-initialisation.
  - `LeaseMode::kWaitForLease` makes a hot standby.
- **`Subscriber`**: a read-only reader whose non-blocking `try_read` reports:
  - `kMessage`: validated against torn reads;
  - `kGap{first, last}`: exact;
  - `kEpochChange`;
  - `kPublisherDead`: detected by probing the lease, rate-limited, with no false positives;
  - `kEmpty`.
- **`inspect` and `list_channels`**: read-only channel inspection. **`revenant-stat`** is a CLI on top of them.
- **Core**: power-of-two ring geometry, the byte-exact v1 layout with a layout hash, hostile-input header validation, and the per-slot seqlock. All memory orderings are confined to `core/atomics.hpp`.
- **Tests:**
  - unit, integration and threaded stress suites;
  - a crash suite with real `SIGKILL`s at seven fault points, plus hot-standby takeover;
  - CI on GCC 13 and 14 and Clang 18, on x86-64 and arm64, with ASan, UBSan and TSan.
- **Benchmarks**: cross-process latency, fan-out cost and recovery time against Unix domain sockets, with a written methodology.
- **Examples**: a ticker publisher and subscriber, and `kill_demo.sh`, which CI runs on every push.

[0.1.0]: https://github.com/ellay21/Revenant/releases/tag/v0.1.0

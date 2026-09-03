# Revenant

> Crash-safe shared-memory market-data fan-out for C++20 on Linux.

**Status: pre-alpha.** Implementation is in progress.

One publisher process broadcasts small fixed-size messages (quotes, trades, book updates) to any number of subscriber processes on the same host. It does this through a lock-free ring in shared memory, so each message costs one write no matter how many subscribers there are.

Most shared-memory rings do not say what happens when a process dies part-way through an operation. Revenant defines it and tests it:

| | Guarantee | Mechanism |
|---|---|---|
| **G1** | **No torn reads.** A subscriber never delivers a message that was half-written or overwritten while it was being read. | Per-slot seqlock: copy, then re-validate |
| **G2** | **No silent loss.** Every message a subscriber misses is reported as an exact `Gap{first, last}`. | Monotonic 64-bit sequence numbers |
| **G3** | **Bounded death detection.** When the publisher dies, idle subscribers learn about it within a set interval. They never wait forever. | The publisher holds a kernel file lock on the segment (the *lease*), and the kernel releases it on death |
| **G4** | **Single writer, clean resume.** At most one publisher is live. A restarted or hot-standby publisher continues the sequence, and subscribers get an explicit `EpochChange`. | The same lease, plus O(1) ring recovery |

**Non-goals:** multiple publishers, cross-host transport, persistence, back-pressure, and zero-copy reads.

## Building

Requirements: Linux, a C++20 compiler (GCC 12+ or Clang 16+), CMake 3.25+ and Ninja. GoogleTest is downloaded at configure time, and only when tests are built.

```bash
cmake --preset dev               # Debug with assertions; also: release, asan, tsan
cmake --build --preset dev
ctest --preset dev
```

The [dev container](.devcontainer/devcontainer.json) gives you the same Ubuntu 24.04 toolchain that CI uses.

## License

Apache-2.0

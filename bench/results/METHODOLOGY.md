# Benchmark methodology

Benchmarks are not tests. They are built only by the `bench` preset (Release, no tests, no fault points), and CI only checks that they build and run. Numbers from shared CI runners are never published.

```bash
cmake --preset bench && cmake --build --preset bench
build/bench/bench/bench_latency_pingpong --iterations=1000000
build/bench/bench/bench_fanout --messages=200000 --max-subscribers=8
build/bench/bench/bench_recovery --iterations=1000
```

## What each benchmark measures

| Binary | Measures | Baseline |
|---|---|---|
| `bench_latency_pingpong` | One-way latency as round-trip time / 2 between two processes pinned to different CPUs, with 64-byte messages. Revenant uses two channels, ping and pong. | The same exchange over `socketpair(AF_UNIX, SOCK_SEQPACKET)` |
| `bench_fanout` | One publisher and N subscriber processes, with messages paced at a fixed interval. Reports the publisher's cost per message and the slowest subscriber's delivery latency (publish timestamp to receipt, using `CLOCK_MONOTONIC`, which is shared across processes). | A Unix-socket publisher that must `send` every message to each of the N sockets |
| `bench_recovery` | **Death detection:** `SIGKILL` sent → an idle subscriber returns `kPublisherDead`, with 1 ms and 100 µs probe intervals. **Takeover:** `SIGKILL` sent → the subscriber receives the first message of the next epoch from a hot standby blocked in `Publisher::create(kWaitForLease)`. | — |

## Rules

- **Distributions, never means.** Every table reports the sample count, p50, p90, p99, p99.9 and max.
- **Exact percentiles.** Every sample goes into a preallocated vector, which is sorted once (nearest-rank).
- **Ping-pong is closed-loop.** The next ping is sent only after the previous pong arrives. That rules out queueing and coordinated omission, but it measures latency at a load the benchmark itself chooses.
- **Fan-out is open-loop and paced.** A slow subscriber falls behind rather than slowing the publisher: Revenant reports a gap, while a socket publisher blocks.
- **Recovery includes the kernel.** The clock starts when `kill` is called, so it includes the kernel tearing the process down, which is what a real crash costs.
- **Every result names its environment:** CPU model, online CPUs, kernel, toolchain, and whether cores were isolated. Results are stored as `results/<host>-<date>.md`.

## Known limits

- On a machine with fewer cores than busy processes, spinning subscribers are descheduled. Their delivery latency then measures the scheduler, not the transport. Publisher cost is unaffected, because it never waits for anyone.
- Shared VMs such as Codespaces have noisy neighbours, and the tail percentiles show it. Treat such numbers as indicative.
- `max` is a single sample, and it is usually a scheduling or page-fault outlier.

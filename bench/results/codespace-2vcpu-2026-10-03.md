# Results: GitHub Codespace, 2 vCPU — 2026-10-03

**Indicative only.** This is a shared VM with two vCPUs and no isolated cores. See [METHODOLOGY.md](METHODOLOGY.md).

| | |
|---|---|
| CPU | AMD EPYC 7763 (2 vCPUs online, shared VM) |
| Kernel | Linux 6.8.0-1064-azure |
| Toolchain | GCC 15.2, Release (`-O3 -DNDEBUG`), `bench` preset |
| Isolation | none (no `isolcpus`, no governor control) |

## One-way latency: cross-process ping-pong, 64-byte messages

`bench_latency_pingpong --iterations=200000`, times in ns:

| Transport | Samples | p50 | p90 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| **Revenant** (shared memory) | 200,000 | **95** | 120 | **130** | 180 | 1,004,951 |
| Unix socket (`SOCK_SEQPACKET`) | 200,000 | 6,892 | 9,117 | 14,557 | 22,492 | 1,215,023 |

Revenant is about **70× faster at p50** and **110× faster at p99**. A message costs one cache-line handoff instead of two syscalls and two context switches.

## Fan-out: publisher cost per message vs subscriber count

`bench_fanout --messages=100000 --max-subscribers=4`, messages paced every 5 µs, times in ns:

| Transport | N | Publish p50 | Publish p99 |
|---|---:|---:|---:|
| **Revenant** | 1 | **50** | 80 |
| **Revenant** | 2 | **40** | 51 |
| **Revenant** | 4 | **40** | 60 |
| Unix socket | 1 | 2,214 | 12,002 |
| Unix socket | 2 | 4,388 | 24,365 |
| Unix socket | 4 | 11,170 | 58,600 |

The Revenant publisher writes each message once, so its cost is **flat in N**. The socket publisher's cost grows linearly, because it sends once per subscriber.

**Delivery latency is not shown.** With one pacing publisher and N spinning subscribers on two vCPUs, the subscribers are descheduled, so their latency measures the scheduler. No subscriber saw a gap in any run. Rerun on a machine with N + 1 free cores to measure delivery latency.

## Recovery after `SIGKILL`

`bench_recovery --iterations=200`, times in µs:

| Event | Samples | p50 | p90 | p99 | max |
|---|---:|---:|---:|---:|---:|
| Death detected, probe every 1 ms | 200 | **808** | 1,020 | 2,818 | 3,771 |
| Death detected, probe every 100 µs | 200 | **111** | 219 | 2,273 | 7,679 |
| Hot-standby takeover → first message of the new epoch | 200 | **141** | 1,723 | 3,325 | 4,907 |

- **Detection tracks the probe interval,** as designed: p50 is about the interval plus kernel teardown.
- **Takeover needs no timeout at all.** The kernel hands the lease to the blocked standby the moment the dead publisher's descriptors close. The standby then recovers in O(1) and publishes.
- **The tails come from the shared 2-vCPU VM:** the killed process, the standby and the spinning subscriber compete for two cores.

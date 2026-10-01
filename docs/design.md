# Design

This document explains *why* Revenant works. The byte-exact layout is specified separately in [wire-format.md](wire-format.md).

## 1. The shape of the problem

One publisher process, typically a feed handler, broadcasts small, bounded messages (quotes, trades, book updates) to any number of subscriber processes on the same host. Three requirements shape the design:

- **Subscribers must never slow the publisher.** A slow subscriber falls behind and is told exactly what it missed. It never applies back-pressure.
- **A subscriber must never deliver a half-written message.** This must hold even though the publisher keeps overwriting the ring while subscribers read it.
- **A process dying at any instruction must not wedge or corrupt anyone else.** So there are no locks anywhere a process could die holding one.

## 2. Fixed slots, one seqlock word each

The ring is `slot_count` fixed-size slots. Message `s` lives in slot `s & (slot_count − 1)`, so finding it is a mask, not a search.

| Considered | Why not |
|---|---|
| Variable-length byte ring (Aeron and Disruptor-style log) | A lapped reader cannot jump to an arbitrary position without landing mid-frame. It would need a resync index and padding frames: about three times the code and test surface. |
| Fixed slots with a mutex each | A process killed while holding the mutex wedges the slot forever. |

Each slot's first word, `seq_word`, says both *which* message the slot holds and *whether* it is complete. `0` means never written, `2s − 1` means message `s` is being written, and `2s` means message `s` is committed. The word only ever increases, so a single comparison against the word a reader expects classifies the slot:

| Slot word vs. expected `e` | Meaning |
|---|---|
| `< 2e − 1` | Not written yet: the slot still holds an older message |
| `= 2e − 1` | Being written right now |
| `= 2e` | Ready: copy it |
| `> 2e` | Lapped: the publisher has already reused the slot for a newer message |

The cost is a payload cap of `slot_size − 16` bytes (up to 4080). Normalised market-data messages fit comfortably.

## 3. The slot protocol

This is the seqlock from Hans Boehm, *Can Seqlocks Get Along with Programming Language Memory Models?* (2012), applied per slot. The implementation is [`core/seqlock.hpp`](../include/revenant/core/seqlock.hpp).

```text
write_slot(s, payload)                               read_slot(e) → out
  W1  seq_word ← 2s − 1        relaxed                 R1  v1 ← seq_word              acquire
  W2  fence                    release                     classify v1 against e (table above)
  W3  meta, payload words      relaxed atomic          R2  meta, payload words → out  relaxed atomic
  W4  seq_word ← 2s            release                 R3  fence                      acquire
                                                       R4  v2 ← seq_word              relaxed
                                                           v2 ≠ v1 → torn: discard out
```

### Why the orderings are sufficient

| Step | Order | Reason |
|---|---|---|
| W4 → R1 | release → acquire | A reader that sees `2s` also sees every W3 store of message `s`. |
| W2 → R3 | release fence → acquire fence | If any R2 load observes *any* W3 store of a newer message, the fences synchronise. W1 then happens-before R4, so R4 reads the odd word or later, and the copy is rejected. |
| W3, R2 | relaxed atomic | These run concurrently by design. Ordering comes from the fences and the seq word. Atomicity is still required, because plain accesses would be a data race. |

There are only two cases. Either every R2 load saw values of message `s`, and the copy is a consistent snapshot. Or some R2 load saw a newer value, and R4 is guaranteed to see the changed word.

**What it compiles to.** On x86-64 every operation above is a plain `mov`, and the fences only stop the compiler from reordering. On AArch64, W4 becomes `stlr`, R1 becomes `ldar`, and the fences become `dmb`. x86 hides missing orderings that ARM exposes, which is why CI runs an arm64 job.

## 4. Copy out, then validate: no zero-copy reads

A reader can know its read was clean only *after* reading, at R4. Two consequences follow.

- **There is no zero-copy view of a slot.** A user handler holding a pointer into the slot could act on bytes that later turn out to be torn. Making that safe needs a "validate after use, no side effects" contract, which is the easiest way for a user to shoot themselves in the foot. Instead `read_slot` copies into the caller's buffer, and the buffer is meaningful only if the read returns *committed*.
- **There is no `memcpy` of shared bytes.** A plain read racing a plain write is a data race, and that is undefined behaviour even if the result is discarded. `volatile` gives no inter-thread guarantees. So meta and payload move as relaxed 8-byte atomic words. On x86-64 and AArch64 those are ordinary loads and stores, and the program stays race-free under the standard, so ThreadSanitizer runs without suppressions. The price is that compilers will not vectorise the copy. For a 240-byte payload that is 30 word loads.

## 5. Memory-ordering discipline

Every atomic access goes through [`core/atomics.hpp`](../include/revenant/core/atomics.hpp). Its functions spell the ordering in their names: `load_acquire`, `store_release`, `fence_release`, `store_words_relaxed`, and so on. That file is the only one allowed to mention `std::memory_order`, and `scripts/lint.sh` fails the build otherwise. Reviewing the concurrency of the whole library therefore means reading the call sites labelled W1–W4 and R1–R4 against the table above.

**Read-only mappings.** Subscribers map the segment `PROT_READ`. `std::atomic_ref<T>` requires a non-`const` object, so loads go through a `const_cast` in exactly one place. This is safe because a lock-free 8-byte atomic load never writes, which is enforced with `static_assert(std::atomic_ref<T>::is_always_lock_free)`. C++26's `atomic_ref<const T>` removes the need for the cast.

**Mapped memory and object lifetime.** The wire structs are implicit-lifetime types: trivially copyable aggregates with no padding. Code views mapped bytes through them, and touches every field that can change after initialisation only through `atomic_ref`. C++23's `std::start_lifetime_as` is the standard spelling of this established practice.

## 6. The publisher lease

At most one publisher may write a channel, and subscribers must learn within a bounded time that it has died. Both requirements are met by a single kernel object. The publisher holds an exclusive **open-file-description (OFD) lock** (`fcntl(F_OFD_SETLK)`) on the whole segment file. It is released when the last descriptor of the holding open file description closes, and that happens when the process exits by *any* means, including `SIGKILL` and the OOM killer. The implementation is [`platform/lease.hpp`](../include/revenant/platform/lease.hpp).

| Considered | Problem |
|---|---|
| PID in shared memory + `kill(pid, 0)` | PIDs are reused. A zombie still answers `kill(pid, 0)`, so it looks alive. |
| PID + start time + heartbeat | Correct, but a silent publisher is ambiguous: dead, stopped, or just idle? It also needs a timeout tuned against false positives. |
| Robust `pthread_mutex` in shared memory | Probing requires `trylock`, which writes, and subscribers map the segment read-only. An `EOWNERDEAD` recovery protocol is also needed. |
| POSIX record lock (`F_SETLK`) | Owned by the *process*. A second open in the same process silently succeeds, and closing *any* descriptor to the file drops the lock. |
| `flock` | Correct ownership, but it cannot be queried without acquiring it. |
| **OFD lock** | Owned by the open file description, can be queried with `F_OFD_GETLK` from a read-only descriptor, and released by the kernel on death. |

**Consequences**
- **Single writer (INV1) is enforced by the kernel.** A second publisher, even a second `open` in the same process, fails to lock.
- **Liveness is definitive.** A subscriber that sees no new messages probes the lock (`F_OFD_GETLK`, one syscall, rate-limited). A held lock means the publisher is alive; a free lock means it is gone. There are no false positives and no PID-reuse races. A zombie publisher has already closed its descriptors, so its lease is already free.
- **Probing never acquires,** so it can never make a starting publisher fail.
- **A clean exit and a crash look the same** to subscribers. This is intentional.
- **Caveat:** a child `fork`ed without `exec` shares the open file description and keeps the lease alive. Publishers must open the segment `O_CLOEXEC` (they do) and should not fork without exec.

The integration tests pin each of these semantics. Swapping the OFD calls for POSIX `F_SETLK`/`F_GETLK` makes three of them fail.

## 7. Publisher start-up and recovery

`Publisher::create` runs the same steps whether the channel is new, cleanly abandoned, or left behind by a crash. The implementation is [`channel/publisher.cpp`](../src/channel/publisher.cpp).

```text
open or create the file (O_CLOEXEC, mode 0600)
take the lease                      F_OFD_SETLK, or F_OFD_SETLKW for a hot standby
if the file is empty, or validate_header says incomplete (magic == 0 or < 256 bytes):
    resize to 256 + N × slot_size     never through zero: a waiting subscriber may have it mapped
    initialize_segment                zero everything, write the header, then magic (release)
else:
    validate; a foreign segment or another geometry is refused and never overwritten
recover:
    h = head (acquire)
    if slot(h + 1) holds committed(h + 1): h += 1, head ← h (release)    died between W4 and W5
    next sequence = h + 1
epoch ← epoch + 1 (release)
```

Each publish ends with **W5**, `head ← s` (release). This lets a subscriber attach at the live edge and find a resume point after an overrun.

**Why this is correct:**
- **Only the lease holder writes.** The kernel released the previous holder's lease only after the holder died, so every store it made is visible to us.
- **INV3: at most one committed message is ahead of `head`.** W5 follows W4 directly, so roll-forward checks a single slot.
- **A slot left mid-write (W1–W3) was never committed,** so no subscriber delivered it. The new publisher reuses that sequence for its first message. There is no duplicate, and the slot is repaired simply by writing it normally.
- **Recovery is idempotent.** Dying during recovery leaves state that the next publisher recovers the same way.
- **An incomplete segment is always abandoned.** We hold the lease, so its creator is dead. Re-initialising it is safe. A segment with *foreign* magic is never touched.

## 8. Subscribers: gaps, epochs and death

`Subscriber::try_read` never blocks, and it returns exactly one event per call.

| Result | When |
|---|---|
| `kMessage` | The next message was copied out and validated. |
| `kEmpty` | Nothing new yet. |
| `kGap{first, last}` | Those messages were overwritten before they were read. The range is exact (G2). |
| `kEpochChange{e}` | A new publisher took over. This is returned before its first message (INV6), and it is not a message: the next call returns that message. |
| `kPublisherDead` | No process holds the lease. It is reported once per death. |

**Joining and falling behind.** A subscriber attaches at the live edge, `head + 1`. When it finds its slot lapped or torn, it resumes **half a ring behind** the writer:
- The oldest slot is the next one the writer overwrites, so resuming there would lap again at once.
- The newest slot loses the most data.
- Half a ring keeps recent data and leaves `slot_count / 2` messages of slack.

`head` may lag the slot that was observed by one message (INV3), so the observed sequence bounds it from below.

**Liveness without heartbeats.** Only while it has nothing to read, the subscriber:
- loads `epoch`, which a new publisher bumps before publishing anything;
- at most once per `liveness_probe_interval` (default 1 ms), asks the kernel whether any process holds the lease (`F_OFD_GETLK`, one syscall).

The hot path pays for neither. Detection latency is bounded by the probe interval plus the caller's polling period, and there are no false positives: a held lease means a live publisher.

## 9. Failure semantics

The crash suite ([`tests/crash/crash_kill_points.cpp`](../tests/crash/crash_kill_points.cpp)) forks a publisher and kills it with a real `SIGKILL` on the Nth hit of each fault point. N covers the first message, the second, the last slot of the first lap, the first of the second lap, and the third lap. The kernel then cleans up exactly as it would after an OOM kill. Fault points exist only in test builds; the release library contains none.

| Point | Publisher killed… | Subscriber observes | Next publisher |
|---|---|---|---|
| **F1** after W1 | slot marked "being written" | an exact prefix up to message `n − 1`, then `PublisherDead`; never a torn read | resumes at `n`; message `n` was never delivered, so there is no duplicate |
| **F2** mid-payload | half the payload written | same as F1 | same as F1 |
| **F3** before W4 | payload complete, not committed | same as F1 | same as F1 |
| **F4** after W4 | committed, `head` stale | message `n` normally: readers trust the slot word, not `head` | rolls `head` forward to `n` (INV3), resumes at `n + 1` |
| **F5** after W5 | message complete | everything up to `n` | resumes at `n + 1` |
| **F6** during recovery | lease held, epoch not bumped | `PublisherDead`; nothing new was written | recovers identically: recovery is idempotent |
| **F7** during initialisation | `magic` not yet published | `attach` fails with `segment_incomplete` (retryable) | re-initialises the abandoned segment |

In every publish-path case the subscriber then receives `EpochChange{2}`, followed by the next sequence. Each test asserts the exact sequence the next publisher resumes at, and checks every byte of every delivered message.

| Other event | Effect |
|---|---|
| A subscriber is killed | None. Nothing waits for subscribers, and they map the segment read-only. |
| N hot standbys are waiting | The kernel grants the lease to exactly one per death. |
| The publisher is a zombie | Its lease is already released, because descriptors close before the zombie state. |
| The publisher is `SIGSTOP`ped | Subscribers see `kEmpty`: it is alive and holds the lease. A heartbeat for stalls is a non-goal. |
| The publisher forks without `exec`, then dies | The child shares the open file description, so the lease lives on until the child exits. |
| A segment from another build, or a hostile one | `layout_mismatch`, `version_mismatch`, `bad_magic` or `segment_corrupt`; never a crash or an out-of-bounds read. |

**Mutation check.** Disabling the INV3 roll-forward makes all five F4 cases fail.

## 10. What the tests do and do not prove

- **Single-threaded unit tests prove the protocol's decisions.** A test seam runs between the copy and the re-check and rewrites the slot "during" the read, so torn reads are exercised deterministically. Mutation checks confirm the tests fail if the re-check is removed.
- **x86-64 never reorders a store with an older store,** so a missing `release` can pass every x86 test. The arm64 CI job and ThreadSanitizer reduce that risk; they do not eliminate it. Exhaustive weak-memory checking (herd7, GenMC) is future work.

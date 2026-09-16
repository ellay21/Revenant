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

## 6. What the tests do and do not prove

- **Single-threaded unit tests prove the protocol's decisions.** A test seam runs between the copy and the re-check and rewrites the slot "during" the read, so torn reads are exercised deterministically. Mutation checks confirm the tests fail if the re-check is removed.
- **x86-64 never reorders a store with an older store,** so a missing `release` can pass every x86 test. The arm64 CI job and ThreadSanitizer reduce that risk; they do not eliminate it. Exhaustive weak-memory checking (herd7, GenMC) is future work.

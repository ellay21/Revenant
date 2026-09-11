# Wire format — version 1 (normative)

A channel is one shared-memory file, `/dev/shm/revenant.<name>`. The publisher and its subscribers may be built from different commits, so this layout is a compatibility contract. [`include/revenant/core/layout.hpp`](../include/revenant/core/layout.hpp) mirrors it with `static_assert`s, and [`tests/unit/test_layout.cpp`](../tests/unit/test_layout.cpp) pins every offset.

**Changing any byte described here requires bumping `kWireVersion`.**

## Conventions

- **Byte order is native little-endian** (x86-64 and aarch64 Linux). All participants share one host, so bytes are never converted.
- **Every field is naturally aligned.**
- **Shared atomic fields are 4 or 8 bytes and must be lock-free** (`std::atomic_ref<T>::is_always_lock_free`). A lock-free load never writes to memory, which is what makes atomic loads legal on a subscriber's read-only mapping.
- **A line is 64 bytes** (`kWireLine`). This is a fixed wire constant, not `std::hardware_destructive_interference_size`, which can change with compiler flags.

## Segment map

| Offset | Size | Region |
|---|---|---|
| 0 | 64 | `SegmentHeader`: immutable once `magic` is published |
| 64 | 64 | `ControlBlock`: line holding `head` |
| 128 | 64 | `ControlBlock`: line holding `epoch` |
| 192 | 64 | Reserved (zero) |
| 256 | `slot_count × slot_size` | Slots |

The file size is exactly `256 + slot_count × slot_size`.

## `SegmentHeader` (offset 0)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u64`, atomic | `magic` | `0x5245564E414E5431` ("REVNANT1") once initialised, `0` before. **Stored last, with release ordering.** |
| 8 | `u32` | `wire_version` | `1` |
| 12 | `u32` | `slot_size` | Bytes per slot: a power of two, 64 to 4096 |
| 16 | `u32` | `slot_count` | A power of two, 2 to 2²⁴ |
| 20 | `u32` | `reserved0` | `0` |
| 24 | `u64` | `layout_hash` | Fingerprint of this layout and geometry |
| 32 | `u64` | `created_unix_ns` | `CLOCK_REALTIME` at initialisation; diagnostic only |
| 40 | 24 bytes | `reserved` | `0` |

## `ControlBlock` (offset 64)

| Segment offset | Type | Field | Written by | Meaning |
|---|---|---|---|---|
| 64 | `u64`, atomic | `head` | Publisher | Last sequence whose publication completed; `0` means none. At most one committed message can be ahead of `head`. |
| 128 | `u32`, atomic | `epoch` | Publisher | Incremented by every publisher that acquires the lease; `0` means no publisher has started yet. |

Each field owns its own line, so a store to one never invalidates the line holding the other or the header.

## Slot (offset `256 + i × slot_size`)

| Offset | Type | Field | Meaning |
|---|---|---|---|
| 0 | `u64`, atomic | `seq_word` | `0`: never written. `2s − 1`: message `s` is being written. `2s`: message `s` is committed. |
| 8 | `u64`, relaxed atomic | `meta` | `(epoch << 32) \| length`, protected by the slot's seqlock |
| 16 | `slot_size − 16` bytes | `payload` | Accessed as relaxed atomic 8-byte words; bytes beyond `length` are unspecified |

- **Slot of sequence `s`:** `i = s & (slot_count − 1)`.
- **Sequences** start at 1 and never wrap. The largest is 2⁶³ − 1; at 10⁹ messages per second that is about 292 years away.
- **A slot's `seq_word` only increases:** `0 < 2s − 1 < 2s < 2(s + slot_count) − 1`. Comparing a slot's word with the one a reader expects therefore tells "not written yet" apart from "overwritten by a later lap".
- **Payload capacity** is `slot_size − 16`: 48 to 4080 bytes.

## Versioning

- The wire version changes only when the layout changes incompatibly. There is no partial compatibility: a v1 reader refuses any other version.
- The library version (SemVer) is independent of the wire version.

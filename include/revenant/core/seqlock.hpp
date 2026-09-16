#pragma once

#include <revenant/config.hpp>
#include <revenant/core/atomics.hpp>
#include <revenant/core/layout.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

// Per-slot seqlock: the protocol from Boehm, "Can Seqlocks Get Along with Programming Language
// Memory Models?" (2012). Labels W1-W4 and R1-R4 match docs/design.md.
// Meta and payload are relaxed atomic words: a writer may be storing them while a reader loads
// them, and plain accesses would be a data race (undefined behaviour). The reader therefore
// copies out and re-validates; it never hands out a pointer into the slot.

namespace revenant::core {

enum class SlotState : std::uint8_t {
  kCommitted,   ///< Copy is valid: the slot held `expected` unchanged across the whole copy.
  kNotYet,      ///< Slot holds an older message or was never written.
  kInProgress,  ///< Slot word is writing_word(expected): the writer is mid-message.
  kLapped,      ///< Slot already holds a newer message; seen before copying.
  kTorn,        ///< Slot changed during the copy; the output buffer is garbage.
  kCorrupt,     ///< Stable slot whose length exceeds the capacity; nothing was copied.
};

struct SlotRead {
  SlotState state;
  std::uint64_t observed_seq;  ///< Sequence the slot's word referred to; 0 if never written.
  std::uint32_t epoch;         ///< Valid for kCommitted only.
  std::uint32_t length;        ///< Valid for kCommitted and kCorrupt.
};

namespace detail {

[[nodiscard]] inline SlotHeader& slot_header(std::byte* slot) noexcept {
  REVENANT_ASSERT(reinterpret_cast<std::uintptr_t>(slot) % alignof(SlotHeader) == 0);
  return *reinterpret_cast<SlotHeader*>(slot);
}

[[nodiscard]] inline const SlotHeader& slot_header(const std::byte* slot) noexcept {
  REVENANT_ASSERT(reinterpret_cast<std::uintptr_t>(slot) % alignof(SlotHeader) == 0);
  return *reinterpret_cast<const SlotHeader*>(slot);
}

}  // namespace detail

/// Publishes `payload` as message `seq` (W1-W4). Never blocks and never reads shared state.
/// Narrow contract: the caller is the only writer, `slot` is 8-byte aligned,
/// 1 <= seq <= kMaxSequence, and payload.size() <= capacity.
inline void write_slot(std::byte* slot, std::uint32_t capacity, std::uint64_t seq,
                       std::uint32_t epoch, std::span<const std::byte> payload) noexcept {
  REVENANT_ASSERT(payload.size() <= capacity);
  SlotHeader& header = detail::slot_header(slot);
  const auto length = static_cast<std::uint32_t>(payload.size());

  atomics::store_relaxed(header.seq_word, writing_word(seq));     // W1
  atomics::fence_release();                                       // W2
  atomics::store_relaxed(header.meta, pack_meta(epoch, length));  // W3
  atomics::store_words_relaxed(slot + kSlotHeaderSize, payload);  // W3
  atomics::store_release(header.seq_word, committed_word(seq));   // W4
}

/// Default for read_slot's test seam: does nothing and compiles away.
struct NoHook {
  constexpr void operator()() const noexcept {}
};

/// Copies message `expected` into `out` if the slot holds it, and validates the copy (R1-R4).
/// `out` holds the message only when the state is kCommitted, and is never written beyond
/// min(length, capacity). `between_copy_and_recheck` runs after the copy and before the
/// re-check, so a single-threaded test can make a torn read happen deterministically.
/// Wide contract on slot contents. Narrow contract: `slot` is 8-byte aligned,
/// 1 <= expected <= kMaxSequence, and out.size() >= capacity.
template <typename Hook = NoHook>
[[nodiscard]] SlotRead read_slot(const std::byte* slot, std::uint32_t capacity,
                                 std::uint64_t expected, std::span<std::byte> out,
                                 Hook&& between_copy_and_recheck = {}) noexcept {
  REVENANT_ASSERT(out.size() >= capacity);
  const SlotHeader& header = detail::slot_header(slot);

  const std::uint64_t v1 = atomics::load_acquire(header.seq_word);  // R1
  if (v1 < writing_word(expected)) {
    return {SlotState::kNotYet, sequence_of(v1), 0, 0};
  }
  if (v1 == writing_word(expected)) {
    return {SlotState::kInProgress, expected, 0, 0};
  }
  if (v1 != committed_word(expected)) {
    return {SlotState::kLapped, sequence_of(v1), 0, 0};
  }

  const std::uint64_t meta = atomics::load_relaxed(header.meta);  // R2
  const std::uint32_t length = meta_length(meta);
  const std::uint32_t copy_length = length <= capacity ? length : 0;
  atomics::load_words_relaxed(slot + kSlotHeaderSize, out.first(copy_length));  // R2

  atomics::fence_acquire();  // R3
  between_copy_and_recheck();
  const std::uint64_t v2 = atomics::load_relaxed(header.seq_word);  // R4
  if (v2 != v1) {
    return {SlotState::kTorn, sequence_of(v2), 0, 0};
  }
  if (length > capacity) {
    return {SlotState::kCorrupt, expected, 0, length};
  }
  return {SlotState::kCommitted, expected, meta_epoch(meta), length};
}

/// Acquire load of a slot's seq_word, for publisher recovery and observers.
[[nodiscard]] inline std::uint64_t load_seq_word(const std::byte* slot) noexcept {
  return atomics::load_acquire(detail::slot_header(slot).seq_word);
}

}  // namespace revenant::core

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Thin RAII wrapper over a named POSIX shared-memory object (/dev/shm/revenant.<name>).
// No protocol knowledge lives here. Failures throw std::system_error: a missing segment is
// revenant::errc::channel_not_found, anything else carries the errno.

namespace revenant::platform {

enum class Access : std::uint8_t { kReadWrite, kReadOnly };

/// [A-Za-z0-9._-]{1,200}, not starting with '.', so a name can never escape or hide in /dev/shm.
[[nodiscard]] bool is_valid_channel_name(std::string_view name) noexcept;

class ShmSegment {
 public:
  /// Opens `name` read-write, creating it empty (mode 0600) if absent. Not mapped yet.
  /// An invalid name is errc::invalid_name.
  [[nodiscard]] static ShmSegment open_or_create(std::string_view name);

  /// Opens an existing `name`. Not mapped yet.
  [[nodiscard]] static ShmSegment open(std::string_view name, Access access);

  ShmSegment(ShmSegment&& other) noexcept;
  ShmSegment& operator=(ShmSegment&& other) noexcept;
  ShmSegment(const ShmSegment&) = delete;
  ShmSegment& operator=(const ShmSegment&) = delete;
  ~ShmSegment();

  /// Sets the file size to exactly `bytes` with every page allocated now. On tmpfs, ftruncate
  /// alone makes a sparse file, so a full /dev/shm would surface later as SIGBUS on first
  /// write; posix_fallocate reports ENOSPC here instead.
  /// Narrow contract: opened read-write and not mapped.
  void reserve(std::uint64_t bytes);

  /// Maps the whole file MAP_SHARED; a read-only mapping turns stray writes into SIGSEGV.
  /// Narrow contract: not already mapped.
  void map(Access access);
  void unmap() noexcept;

  [[nodiscard]] std::uint64_t file_size() const;
  [[nodiscard]] std::span<std::byte> bytes() const noexcept { return {data_, size_}; }
  [[nodiscard]] bool is_mapped() const noexcept { return data_ != nullptr; }
  [[nodiscard]] int fd() const noexcept { return fd_; }

 private:
  explicit ShmSegment(int fd) noexcept : fd_(fd) {}
  void close() noexcept;

  int fd_ = -1;
  std::byte* data_ = nullptr;
  std::size_t size_ = 0;
};

/// Removes the name; existing mappings stay valid. A missing name is not an error.
void unlink_channel(std::string_view name);

}  // namespace revenant::platform

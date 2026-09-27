#include <revenant/config.hpp>
#include <revenant/errors.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

namespace revenant::platform {
namespace {

std::string object_name(std::string_view name) {
  std::string path{"/revenant."};
  path.append(name);
  return path;
}

[[noreturn]] void throw_errno(int error, const std::string& what) {
  throw std::system_error(error, std::system_category(), what);
}

int open_object(std::string_view name, int flags) {
  if (!is_valid_channel_name(name)) {
    throw std::system_error(revenant::errc::invalid_name, "channel name");
  }
  const std::string path = object_name(name);
  const int fd = ::shm_open(path.c_str(), flags | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0) {
    if (errno == ENOENT) {
      throw std::system_error(revenant::errc::channel_not_found, "shm_open " + path);
    }
    throw_errno(errno, "shm_open " + path);
  }
  return fd;
}

}  // namespace

bool is_valid_channel_name(std::string_view name) noexcept {
  constexpr std::size_t kMaxLength = 200;
  if (name.empty() || name.size() > kMaxLength || name.front() == '.') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '_' || c == '-';
  });
}

ShmSegment ShmSegment::open_or_create(std::string_view name) {
  return ShmSegment{open_object(name, O_RDWR | O_CREAT)};
}

ShmSegment ShmSegment::open(std::string_view name, Access access) {
  return ShmSegment{open_object(name, access == Access::kReadWrite ? O_RDWR : O_RDONLY)};
}

ShmSegment::ShmSegment(ShmSegment&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

ShmSegment& ShmSegment::operator=(ShmSegment&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = std::exchange(other.fd_, -1);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

ShmSegment::~ShmSegment() {
  close();
}

void ShmSegment::close() noexcept {
  unmap();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void ShmSegment::reserve(std::uint64_t bytes) {
  REVENANT_ASSERT(!is_mapped());
  // Shrink only to the target, never through zero: a reader that mapped the old file must keep
  // valid pages for every byte the new size still covers, or its next read is SIGBUS.
  if (file_size() > bytes && ::ftruncate(fd_, static_cast<off_t>(bytes)) != 0) {
    throw_errno(errno, "ftruncate");
  }
  // posix_fallocate returns the error number instead of setting errno.
  if (const int error = ::posix_fallocate(fd_, 0, static_cast<off_t>(bytes)); error != 0) {
    throw_errno(error, "posix_fallocate");
  }
}

void ShmSegment::map(Access access) {
  REVENANT_ASSERT(!is_mapped());
  const auto size = static_cast<std::size_t>(file_size());
  if (size == 0) {
    return;
  }
  const int protection = access == Access::kReadWrite ? PROT_READ | PROT_WRITE : PROT_READ;
  void* const address = ::mmap(nullptr, size, protection, MAP_SHARED, fd_, 0);
  if (address == MAP_FAILED) {
    throw_errno(errno, "mmap");
  }
  data_ = static_cast<std::byte*>(address);
  size_ = size;
}

void ShmSegment::unmap() noexcept {
  if (data_ != nullptr) {
    ::munmap(data_, size_);
    data_ = nullptr;
    size_ = 0;
  }
}

std::uint64_t ShmSegment::file_size() const {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    throw_errno(errno, "fstat");
  }
  return static_cast<std::uint64_t>(st.st_size);
}

void unlink_channel(std::string_view name) {
  if (!is_valid_channel_name(name)) {
    throw std::system_error(revenant::errc::invalid_name, "channel name");
  }
  const std::string path = object_name(name);
  if (::shm_unlink(path.c_str()) != 0 && errno != ENOENT) {
    throw_errno(errno, "shm_unlink " + path);
  }
}

}  // namespace revenant::platform

#include <revenant/config.hpp>
#include <revenant/errors.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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

void ShmSegment::resize(std::uint64_t bytes) {
  REVENANT_ASSERT(!is_mapped());
  if (::ftruncate(fd_, static_cast<off_t>(bytes)) != 0) {
    throw_errno(errno, "ftruncate");
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
  const std::string path = object_name(name);
  if (::shm_unlink(path.c_str()) != 0 && errno != ENOENT) {
    throw_errno(errno, "shm_unlink " + path);
  }
}

}  // namespace revenant::platform

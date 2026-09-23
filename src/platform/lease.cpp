#include <revenant/platform/lease.hpp>

#include <fcntl.h>

#include <cerrno>
#include <system_error>

namespace revenant::platform {
namespace {

// Whole file: l_start = 0, l_len = 0. OFD locks require l_pid == 0 on input.
struct flock whole_file(short type) noexcept {
  struct flock lock {};
  lock.l_type = type;
  lock.l_whence = SEEK_SET;
  return lock;
}

}  // namespace

bool try_acquire_lease(int fd) {
  struct flock lock = whole_file(F_WRLCK);
  if (::fcntl(fd, F_OFD_SETLK, &lock) == 0) {
    return true;
  }
  if (errno == EAGAIN || errno == EACCES) {
    return false;
  }
  throw std::system_error(errno, std::system_category(), "fcntl(F_OFD_SETLK)");
}

bool lease_is_held(int fd) {
  struct flock lock = whole_file(F_RDLCK);
  if (::fcntl(fd, F_OFD_GETLK, &lock) != 0) {
    throw std::system_error(errno, std::system_category(), "fcntl(F_OFD_GETLK)");
  }
  return lock.l_type != F_UNLCK;
}

}  // namespace revenant::platform

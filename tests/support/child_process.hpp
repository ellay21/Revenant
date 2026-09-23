#pragma once

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

// Forked children for tests that need a second process: a peer that dies, holds a lock, or is
// killed mid-operation. The destructor kills and reaps, so a failing assertion never leaks one.

namespace revenant::testing {

/// The child's end of two pipes to its parent.
class ChildLink {
 public:
  ChildLink(int to_parent, int from_parent) noexcept
      : to_parent_(to_parent), from_parent_(from_parent) {}

  /// Sends a value to the parent's receive().
  void send(std::uint64_t value) const noexcept {
    [[maybe_unused]] const auto n = ::write(to_parent_, &value, sizeof value);
  }

  /// Blocks until the parent calls go(); false if the parent went away.
  [[nodiscard]] bool wait_go() const noexcept {
    char byte = 0;
    return ::read(from_parent_, &byte, 1) == 1;
  }

 private:
  int to_parent_;
  int from_parent_;
};

class ChildProcess {
 public:
  /// Forks; the child runs body(ChildLink&) and _exits with its return value. Keep the body
  /// small and free of test assertions: it runs in a copy of the test process.
  template <typename Body>
  [[nodiscard]] static ChildProcess spawn(Body&& body) {
    int up[2];
    int down[2];
    if (::pipe2(up, O_CLOEXEC) != 0 || ::pipe2(down, O_CLOEXEC) != 0) {
      throw std::system_error(errno, std::system_category(), "pipe2");
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
      throw std::system_error(errno, std::system_category(), "fork");
    }
    if (pid == 0) {
      ::close(up[0]);
      ::close(down[1]);
      ChildLink link{up[1], down[0]};
      int code = 125;
      try {
        code = std::forward<Body>(body)(link);
      } catch (...) {  // NOLINT(bugprone-empty-catch): reported through the exit code
      }
      ::_exit(code);
    }
    ::close(up[1]);
    ::close(down[0]);
    return ChildProcess{pid, up[0], down[1]};
  }

  ChildProcess(ChildProcess&& other) noexcept
      : pid_(std::exchange(other.pid_, -1)),
        from_child_(std::exchange(other.from_child_, -1)),
        to_child_(std::exchange(other.to_child_, -1)),
        reaped_(other.reaped_) {}
  ChildProcess& operator=(ChildProcess&&) = delete;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  ~ChildProcess() {
    if (pid_ > 0 && !reaped_) {
      ::kill(pid_, SIGKILL);
      int status = 0;
      ::waitpid(pid_, &status, 0);
    }
    if (from_child_ >= 0) {
      ::close(from_child_);
    }
    if (to_child_ >= 0) {
      ::close(to_child_);
    }
  }

  /// Next value the child sent, or nullopt on timeout or if the child exited first.
  [[nodiscard]] std::optional<std::uint64_t> receive(std::chrono::milliseconds timeout) const {
    pollfd p{from_child_, POLLIN, 0};
    if (::poll(&p, 1, static_cast<int>(timeout.count())) != 1) {
      return std::nullopt;
    }
    std::uint64_t value = 0;
    if (::read(from_child_, &value, sizeof value) != static_cast<ssize_t>(sizeof value)) {
      return std::nullopt;
    }
    return value;
  }

  void go() const {
    const char byte = 1;
    [[maybe_unused]] const auto n = ::write(to_child_, &byte, 1);
  }

  void kill(int signal = SIGKILL) const { ::kill(pid_, signal); }

  /// Reaps the child within `timeout`; returns its raw wait status.
  [[nodiscard]] std::optional<int> wait_for(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        reaped_ = true;
        return status;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return std::nullopt;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }

 private:
  ChildProcess(pid_t pid, int from_child, int to_child) noexcept
      : pid_(pid), from_child_(from_child), to_child_(to_child) {}

  pid_t pid_;
  int from_child_;
  int to_child_;
  bool reaped_ = false;
};

[[nodiscard]] inline bool killed_by(std::optional<int> status, int signal) {
  return status && WIFSIGNALED(*status) && WTERMSIG(*status) == signal;
}

[[nodiscard]] inline bool exited_with(std::optional<int> status, int code) {
  return status && WIFEXITED(*status) && WEXITSTATUS(*status) == code;
}

}  // namespace revenant::testing

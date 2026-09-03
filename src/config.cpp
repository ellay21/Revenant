#include <revenant/config.hpp>

#include <cstdio>
#include <cstdlib>

#ifndef REVENANT_VERSION
#error "REVENANT_VERSION must be defined by the build system"
#endif

namespace revenant {

std::string_view version() noexcept {
  return REVENANT_VERSION;
}

namespace detail {

void assertion_failed(const char* expression, const char* file, int line) noexcept {
  std::fprintf(stderr, "revenant: assertion failed: %s (%s:%d)\n", expression, file, line);
  std::abort();
}

}  // namespace detail
}  // namespace revenant

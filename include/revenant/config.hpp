#pragma once

#include <cstddef>
#include <string_view>

namespace revenant {

/// Library version as "MAJOR.MINOR.PATCH".
[[nodiscard]] std::string_view version() noexcept;

/// Padding unit for in-process hot fields. The wire layout never depends on it.
inline constexpr std::size_t kCacheLine = 64;

namespace detail {
[[noreturn]] void assertion_failed(const char* expression, const char* file, int line) noexcept;
}  // namespace detail

}  // namespace revenant

/// Checks a narrow-contract precondition. Debug builds abort with the expression and location;
/// release builds (NDEBUG) never evaluate `cond`, so it must not have side effects.
#if defined(NDEBUG)
// Unevaluated operand: costs nothing, but variables used only in assertions still count as used.
#define REVENANT_ASSERT(cond) static_cast<void>(sizeof(static_cast<bool>(cond)))
#else
#define REVENANT_ASSERT(cond)                     \
  (static_cast<bool>(cond) ? static_cast<void>(0) \
                           : ::revenant::detail::assertion_failed(#cond, __FILE__, __LINE__))
#endif

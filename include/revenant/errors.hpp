#pragma once

#include <system_error>
#include <type_traits>

namespace revenant {

/// Errors reported by Revenant's cold paths (create, attach, validation).
/// Values are stable; the fixed underlying type makes every `int` a valid `errc`.
// NOLINTNEXTLINE(performance-enum-size): std::error_code carries values as int.
enum class errc : int {
  invalid_name = 1,    ///< Channel name empty, too long, or outside [A-Za-z0-9._-].
  invalid_config,      ///< slot_size or slot_count not a power of two, or out of range.
  channel_not_found,   ///< No segment exists with that name.
  publisher_active,    ///< Another publisher holds the channel lease.
  segment_incomplete,  ///< magic == 0 or file too short: initialisation unfinished.
  bad_magic,           ///< Not a Revenant segment.
  version_mismatch,    ///< Wire version differs from this build's.
  layout_mismatch,     ///< layout_hash differs: incompatible build or struct layout.
  geometry_mismatch,   ///< Existing segment differs from the requested configuration.
  segment_corrupt,     ///< Header values are impossible.
};

/// The "revenant" category. Codes with a POSIX equivalent compare equal to that `std::errc`.
[[nodiscard]] const std::error_category& error_category() noexcept;

[[nodiscard]] inline std::error_code make_error_code(errc e) noexcept {
  return {static_cast<int>(e), error_category()};
}

}  // namespace revenant

template <>
struct std::is_error_code_enum<revenant::errc> : std::true_type {};

#include <revenant/errors.hpp>

#include <string>

namespace revenant {
namespace {

class ErrorCategory final : public std::error_category {
 public:
  [[nodiscard]] const char* name() const noexcept override { return "revenant"; }

  [[nodiscard]] std::string message(int value) const override {
    switch (static_cast<errc>(value)) {
      case errc::invalid_name:
        return "invalid channel name";
      case errc::invalid_config:
        return "invalid channel configuration";
      case errc::channel_not_found:
        return "channel not found";
      case errc::publisher_active:
        return "another publisher holds the channel lease";
      case errc::segment_incomplete:
        return "segment is not initialised yet";
      case errc::bad_magic:
        return "not a revenant segment";
      case errc::version_mismatch:
        return "unsupported wire version";
      case errc::layout_mismatch:
        return "segment layout does not match this build";
      case errc::geometry_mismatch:
        return "segment geometry does not match the requested configuration";
      case errc::segment_corrupt:
        return "segment header is corrupt";
    }
    return "unknown revenant error";
  }

  [[nodiscard]] std::error_condition default_error_condition(int value) const noexcept override {
    switch (static_cast<errc>(value)) {
      case errc::invalid_name:
      case errc::invalid_config:
        return std::errc::invalid_argument;
      case errc::channel_not_found:
        return std::errc::no_such_file_or_directory;
      case errc::publisher_active:
        return std::errc::device_or_resource_busy;
      case errc::segment_incomplete:
        return std::errc::resource_unavailable_try_again;
      case errc::bad_magic:
      case errc::version_mismatch:
      case errc::layout_mismatch:
      case errc::geometry_mismatch:
      case errc::segment_corrupt:
        break;
    }
    return {value, *this};
  }
};

}  // namespace

const std::error_category& error_category() noexcept {
  static const ErrorCategory category;
  return category;
}

}  // namespace revenant

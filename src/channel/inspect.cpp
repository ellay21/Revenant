#include <revenant/channel/inspect.hpp>
#include <revenant/core/atomics.hpp>
#include <revenant/core/layout.hpp>
#include <revenant/errors.hpp>
#include <revenant/platform/lease.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>

namespace revenant {

ChannelInfo inspect(std::string_view channel) {
  platform::ShmSegment segment = platform::ShmSegment::open(channel, platform::Access::kReadOnly);
  segment.map(platform::Access::kReadOnly);
  const core::HeaderCheck check = core::validate_header(segment.bytes());
  if (check.error) {
    throw std::system_error(check.error, "revenant channel \"" + std::string{channel} + "\"");
  }
  const std::byte* const base = segment.bytes().data();
  const core::ControlBlock& control = core::control_of(base);
  return ChannelInfo{
      .config = {.slot_size = check.geometry.slot_size, .slot_count = check.geometry.slot_count},
      .epoch = core::atomics::load_acquire(control.epoch),
      .head = core::atomics::load_acquire(control.head),
      .publisher_live = platform::lease_is_held(segment.fd()),
      .created_unix_ns = core::header_of(base).created_unix_ns,
  };
}

std::vector<std::string> list_channels() {
  constexpr std::string_view kPrefix = "revenant.";
  std::vector<std::string> names;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator{"/dev/shm", ec}) {
    const std::string file = entry.path().filename().string();
    if (file.starts_with(kPrefix)) {
      names.push_back(file.substr(kPrefix.size()));
    }
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace revenant

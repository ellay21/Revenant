#include <revenant/channel/inspect.hpp>
#include <revenant/channel/publisher.hpp>
#include <revenant/core/layout.hpp>
#include <revenant/errors.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "support/unique_channel.hpp"

namespace {

using revenant::ChannelConfig;
using revenant::ChannelInfo;
using revenant::errc;
using revenant::Publisher;
using revenant::testing::UniqueChannel;

constexpr ChannelConfig kConfig{.slot_size = 128, .slot_count = 16};

template <typename F>
std::error_code error_of(F&& f) {
  try {
    std::forward<F>(f)();
  } catch (const std::system_error& e) {
    return e.code();
  }
  return {};
}

TEST(Inspect, ReportsGeometryEpochHeadAndALivePublisher) {
  const UniqueChannel channel;
  Publisher publisher = Publisher::create(channel.name(), kConfig);
  const std::array<std::byte, 8> payload{};
  for (int i = 0; i < 5; ++i) {
    publisher.publish(payload);
  }

  const ChannelInfo info = revenant::inspect(channel.name());
  EXPECT_EQ(info.config.slot_size, kConfig.slot_size);
  EXPECT_EQ(info.config.slot_count, kConfig.slot_count);
  EXPECT_EQ(info.epoch, 1U);
  EXPECT_EQ(info.head, 5U);
  EXPECT_TRUE(info.publisher_live);
  EXPECT_GT(info.created_unix_ns, 0U);
}

TEST(Inspect, PublisherGoneIsReportedAndNothingIsDisturbed) {
  const UniqueChannel channel;
  std::optional<Publisher> publisher = Publisher::create(channel.name(), kConfig);
  publisher.reset();
  EXPECT_FALSE(revenant::inspect(channel.name()).publisher_live);

  // Inspecting never takes the lease, so a publisher can still start at once.
  publisher.emplace(Publisher::create(channel.name(), kConfig));
  EXPECT_EQ(publisher->epoch(), 2U);
  EXPECT_TRUE(revenant::inspect(channel.name()).publisher_live);
}

TEST(Inspect, MissingInvalidAndForeignChannelsAreErrors) {
  const UniqueChannel channel;
  EXPECT_EQ(error_of([&] { static_cast<void>(revenant::inspect(channel.name())); }),
            errc::channel_not_found);
  EXPECT_EQ(error_of([&] { static_cast<void>(revenant::inspect("../x")); }), errc::invalid_name);

  { const Publisher publisher = Publisher::create(channel.name(), kConfig); }
  {
    revenant::platform::ShmSegment segment = revenant::platform::ShmSegment::open(
        channel.name(), revenant::platform::Access::kReadWrite);
    segment.map(revenant::platform::Access::kReadWrite);
    revenant::core::header_of(segment.bytes().data()).magic = 0x464C457F;
  }
  EXPECT_EQ(error_of([&] { static_cast<void>(revenant::inspect(channel.name())); }),
            errc::bad_magic);
}

TEST(ListChannels, ListsEveryRevenantChannelByName) {
  const UniqueChannel first;
  const UniqueChannel second;
  { const Publisher a = Publisher::create(first.name(), kConfig); }
  { const Publisher b = Publisher::create(second.name(), kConfig); }

  const std::vector<std::string> names = revenant::list_channels();
  EXPECT_TRUE(std::is_sorted(names.begin(), names.end()));
  EXPECT_NE(std::find(names.begin(), names.end(), first.name()), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), second.name()), names.end());
}

}  // namespace

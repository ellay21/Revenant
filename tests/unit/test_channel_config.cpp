#include <revenant/channel/channel_config.hpp>
#include <revenant/errors.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace {

using namespace std::chrono_literals;
using revenant::ChannelConfig;
using revenant::errc;
using revenant::SubscriberConfig;

TEST(ChannelConfig, DefaultsAreValidWithA240BytePayload) {
  constexpr ChannelConfig config{};
  EXPECT_FALSE(revenant::validate(config));
  EXPECT_EQ(config.slot_size, 256U);
  EXPECT_EQ(config.slot_count, 4096U);
  EXPECT_EQ(config.payload_capacity(), 240U);
  static_assert(config.geometry() == revenant::core::RingGeometry{256, 4096});
}

TEST(ChannelConfig, DesignatedInitialisersReadLikeDocumentation) {
  constexpr ChannelConfig config{.slot_size = 128, .slot_count = 65536};
  EXPECT_FALSE(revenant::validate(config));
  EXPECT_EQ(config.payload_capacity(), 112U);
}

TEST(ChannelConfig, EveryInvalidGeometryIsInvalidConfig) {
  const ChannelConfig invalid[] = {
      {.slot_size = 0, .slot_count = 4096},   {.slot_size = 32, .slot_count = 4096},
      {.slot_size = 96, .slot_count = 4096},  {.slot_size = 8192, .slot_count = 4096},
      {.slot_size = 256, .slot_count = 0},    {.slot_size = 256, .slot_count = 1},
      {.slot_size = 256, .slot_count = 1000}, {.slot_size = 256, .slot_count = 1U << 25},
  };
  for (const ChannelConfig& config : invalid) {
    EXPECT_EQ(revenant::validate(config), errc::invalid_config)
        << config.slot_size << " x " << config.slot_count;
  }
}

TEST(SubscriberConfig, DefaultsProbeEveryMillisecondAndWaitOneSecondToAttach) {
  constexpr SubscriberConfig config{};
  EXPECT_FALSE(revenant::validate(config));
  EXPECT_EQ(config.liveness_probe_interval, 1ms);
  EXPECT_EQ(config.attach_timeout, 1s);
}

TEST(SubscriberConfig, ZeroDurationsAreValid) {
  EXPECT_FALSE(revenant::validate(SubscriberConfig{.liveness_probe_interval = 0ns}));
  EXPECT_FALSE(revenant::validate(SubscriberConfig{.attach_timeout = 0ns}));
}

TEST(SubscriberConfig, NegativeDurationsAreInvalidConfig) {
  EXPECT_EQ(revenant::validate(SubscriberConfig{.liveness_probe_interval = -1ns}),
            errc::invalid_config);
  EXPECT_EQ(revenant::validate(SubscriberConfig{.attach_timeout = -1ms}), errc::invalid_config);
}

}  // namespace

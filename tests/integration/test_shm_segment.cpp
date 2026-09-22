#include <revenant/errors.hpp>
#include <revenant/platform/shm_segment.hpp>

#include <gtest/gtest.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "support/unique_channel.hpp"

namespace {

using revenant::errc;
using revenant::platform::Access;
using revenant::platform::ShmSegment;
using revenant::testing::UniqueChannel;

std::size_t open_fd_count() {
  return static_cast<std::size_t>(std::distance(
      std::filesystem::directory_iterator{"/proc/self/fd"}, std::filesystem::directory_iterator{}));
}

template <typename F>
std::error_code error_of(F&& f) {
  try {
    std::forward<F>(f)();
  } catch (const std::system_error& e) {
    return e.code();
  }
  return {};
}

TEST(ShmSegment, CreateThenReopenReadOnlySeesTheSameBytes) {
  const UniqueChannel channel;
  ShmSegment writer = ShmSegment::open_or_create(channel.name());
  writer.reserve(4096);
  writer.map(Access::kReadWrite);
  ASSERT_EQ(writer.bytes().size(), 4096U);
  writer.bytes()[0] = std::byte{0x2A};
  writer.bytes()[4095] = std::byte{0x7F};

  ShmSegment reader = ShmSegment::open(channel.name(), Access::kReadOnly);
  reader.map(Access::kReadOnly);
  ASSERT_EQ(reader.bytes().size(), 4096U);
  EXPECT_EQ(reader.bytes()[0], std::byte{0x2A});
  EXPECT_EQ(reader.bytes()[4095], std::byte{0x7F});

  writer.bytes()[1] = std::byte{0x11};
  EXPECT_EQ(reader.bytes()[1], std::byte{0x11}) << "both mappings must share the same pages";
}

TEST(ShmSegment, FileIsPrivateToItsOwner) {
  const UniqueChannel channel;
  const ShmSegment segment = ShmSegment::open_or_create(channel.name());
  struct stat st {};
  ASSERT_EQ(::fstat(segment.fd(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600U);
}

TEST(ShmSegment, OpeningAMissingChannelIsChannelNotFound) {
  const UniqueChannel channel;
  EXPECT_EQ(error_of([&] { (void)ShmSegment::open(channel.name(), Access::kReadOnly); }),
            errc::channel_not_found);
  EXPECT_EQ(error_of([&] { (void)ShmSegment::open(channel.name(), Access::kReadWrite); }),
            errc::channel_not_found);
}

TEST(ShmSegment, OpenOrCreateOpensAnExistingSegment) {
  const UniqueChannel channel;
  ShmSegment first = ShmSegment::open_or_create(channel.name());
  first.reserve(8192);
  const ShmSegment second = ShmSegment::open_or_create(channel.name());
  EXPECT_EQ(second.file_size(), 8192U);
}

TEST(ShmSegment, ReservedFileIsZeroFilledFullyMappedAndBackedByPages) {
  const UniqueChannel channel;
  ShmSegment segment = ShmSegment::open_or_create(channel.name());
  EXPECT_EQ(segment.file_size(), 0U);
  segment.map(Access::kReadWrite);
  EXPECT_TRUE(segment.bytes().empty()) << "an empty file maps to an empty span";
  segment.unmap();

  constexpr std::uint64_t kSize = std::uint64_t{3} * 4096;
  segment.reserve(kSize);
  struct stat st {};
  ASSERT_EQ(::fstat(segment.fd(), &st), 0);
  EXPECT_GE(static_cast<std::uint64_t>(st.st_blocks) * 512, kSize)
      << "pages must be allocated now, not on first write (a sparse file would SIGBUS later)";

  segment.map(Access::kReadWrite);
  ASSERT_EQ(segment.bytes().size(), kSize);
  EXPECT_TRUE(std::all_of(segment.bytes().begin(), segment.bytes().end(),
                          [](std::byte b) { return b == std::byte{0}; }));
}

TEST(ShmSegment, ReserveSetsTheExactSizeAndCanShrink) {
  const UniqueChannel channel;
  ShmSegment segment = ShmSegment::open_or_create(channel.name());
  segment.reserve(16384);
  segment.reserve(4096);
  EXPECT_EQ(segment.file_size(), 4096U);
}

// The failure surfaces at create time as an exception, not later as SIGBUS inside publish.
TEST(ShmSegment, ReservingMoreThanTmpfsCanHoldFailsUpFront) {
  const UniqueChannel channel;
  ShmSegment segment = ShmSegment::open_or_create(channel.name());
  const std::error_code ec = error_of([&] { segment.reserve(std::uint64_t{1} << 46); });
  EXPECT_TRUE(ec == std::errc::no_space_on_device || ec == std::errc::file_too_large)
      << ec.message();
}

TEST(ChannelName, AcceptsPortableNames) {
  for (const std::string_view name : {"quotes", "md.equities.us", "feed-1_A", "a", "x."}) {
    EXPECT_TRUE(revenant::platform::is_valid_channel_name(name)) << name;
  }
  EXPECT_TRUE(revenant::platform::is_valid_channel_name(std::string(200, 'n')));
}

TEST(ChannelName, RejectsNamesThatCouldEscapeOrHideInDevShm) {
  for (const std::string_view name : {"", "a/b", "/abs", ".hidden", "..", "sp ace", "tab\t",
                                      "new\nline", "caf\xc3\xa9", "star*"}) {
    EXPECT_FALSE(revenant::platform::is_valid_channel_name(name)) << name;
  }
  EXPECT_FALSE(revenant::platform::is_valid_channel_name(std::string_view{"nul\0x", 5}));
  EXPECT_FALSE(revenant::platform::is_valid_channel_name(std::string(201, 'n')));
}

TEST(ChannelName, EveryEntryPointRejectsAnInvalidName) {
  EXPECT_EQ(error_of([] { (void)ShmSegment::open_or_create("../etc"); }), errc::invalid_name);
  EXPECT_EQ(error_of([] { (void)ShmSegment::open("a/b", Access::kReadOnly); }), errc::invalid_name);
  EXPECT_EQ(error_of([] { revenant::platform::unlink_channel(""); }), errc::invalid_name);
}

TEST(ShmSegmentDeathTest, WritingThroughAReadOnlyMappingCrashes) {
  const UniqueChannel channel;
  ShmSegment writer = ShmSegment::open_or_create(channel.name());
  writer.reserve(4096);
  ShmSegment reader = ShmSegment::open(channel.name(), Access::kReadOnly);
  reader.map(Access::kReadOnly);
  EXPECT_DEATH(reader.bytes()[0] = std::byte{1}, "");
}

TEST(ShmSegment, MovingLeavesTheSourceEmpty) {
  const UniqueChannel channel;
  ShmSegment original = ShmSegment::open_or_create(channel.name());
  original.reserve(4096);
  original.map(Access::kReadWrite);
  const int fd = original.fd();

  ShmSegment moved{std::move(original)};
  EXPECT_EQ(moved.fd(), fd);
  EXPECT_EQ(moved.bytes().size(), 4096U);
  EXPECT_EQ(original.fd(), -1);  // NOLINT(bugprone-use-after-move): checking the moved-from state
  EXPECT_TRUE(original.bytes().empty());  // NOLINT(bugprone-use-after-move)

  const std::size_t before = open_fd_count();
  ShmSegment other = ShmSegment::open_or_create(channel.name());
  EXPECT_EQ(open_fd_count(), before + 1);
  other = std::move(moved);
  EXPECT_EQ(open_fd_count(), before) << "move-assignment must close the overwritten descriptor";
  EXPECT_EQ(other.fd(), fd);
}

TEST(ShmSegment, LeaksNoFileDescriptors) {
  const UniqueChannel channel;
  const std::size_t before = open_fd_count();
  for (int i = 0; i < 100; ++i) {
    ShmSegment segment = ShmSegment::open_or_create(channel.name());
    segment.reserve(4096);
    segment.map(Access::kReadWrite);
    ShmSegment reader = ShmSegment::open(channel.name(), Access::kReadOnly);
    reader.map(Access::kReadOnly);
  }
  EXPECT_EQ(open_fd_count(), before);
}

TEST(ShmSegment, UnlinkRemovesTheNameButKeepsExistingMappingsValid) {
  const UniqueChannel channel;
  ShmSegment segment = ShmSegment::open_or_create(channel.name());
  segment.reserve(4096);
  segment.map(Access::kReadWrite);
  segment.bytes()[7] = std::byte{9};

  revenant::platform::unlink_channel(channel.name());
  EXPECT_EQ(error_of([&] { (void)ShmSegment::open(channel.name(), Access::kReadOnly); }),
            errc::channel_not_found);
  EXPECT_EQ(segment.bytes()[7], std::byte{9});
  EXPECT_NO_THROW(revenant::platform::unlink_channel(channel.name())) << "missing is not an error";
}

}  // namespace

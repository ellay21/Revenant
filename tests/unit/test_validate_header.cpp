#include <revenant/core/layout.hpp>
#include <revenant/errors.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <vector>

namespace {

namespace core = revenant::core;
using core::RingGeometry;
using revenant::errc;

constexpr RingGeometry kGeometry{64, 2};
constexpr std::size_t kSize = 256 + 2 * 64;
constexpr std::uint64_t kCreatedNs = 1'757'000'000'000'000'000;
constexpr std::size_t kCreatedOffset = offsetof(core::SegmentHeader, created_unix_ns);

static_assert(core::segment_size(kGeometry) == kSize);
static_assert(core::layout_hash(kGeometry) == core::layout_hash(kGeometry));

// Heap buffer of exactly `size` bytes, so AddressSanitizer flags any read past the span.
class Segment {
 public:
  explicit Segment(std::size_t size) : bytes_(std::make_unique<std::byte[]>(size)), size_(size) {}
  [[nodiscard]] std::span<std::byte> span() const { return {bytes_.get(), size_}; }

 private:
  std::unique_ptr<std::byte[]> bytes_;
  std::size_t size_;
};

std::vector<std::byte> valid_image() {
  const Segment segment{kSize};
  core::initialize_segment(segment.span(), kGeometry, kCreatedNs);
  return {segment.span().begin(), segment.span().end()};
}

core::HeaderCheck validate(const std::vector<std::byte>& image) {
  const Segment segment{image.size()};
  std::copy(image.begin(), image.end(), segment.span().begin());
  return core::validate_header(segment.span());
}

template <typename T>
void poke(std::vector<std::byte>& image, std::size_t offset, T value) {
  std::memcpy(image.data() + offset, &value, sizeof value);
}

TEST(LayoutHash, Fnv1aMatchesPublishedTestVectors) {
  const auto hash = [](std::string_view text) {
    std::uint64_t h = core::detail::kFnvOffsetBasis;
    for (const char c : text) {
      h = core::detail::fnv1a_byte(h, static_cast<std::uint8_t>(c));
    }
    return h;
  };
  EXPECT_EQ(hash(""), 0xcbf29ce484222325U);
  EXPECT_EQ(hash("a"), 0xaf63dc4c8601ec8cU);
  EXPECT_EQ(hash("foobar"), 0x85944171f73967e8U);
}

TEST(LayoutHash, DependsOnTheGeometry) {
  EXPECT_NE(core::layout_hash({64, 2}), core::layout_hash({128, 2}));
  EXPECT_NE(core::layout_hash({64, 2}), core::layout_hash({64, 4}));
  EXPECT_NE(core::layout_hash({128, 64}), core::layout_hash({64, 128}));
}

// If this value changes, the wire format changed: bump kWireVersion.
TEST(LayoutHash, IsPinnedForWireVersionOne) {
  EXPECT_EQ(core::layout_hash({256, 4096}), 0xaaf2'be61'fbe7'31e0U);
}

TEST(InitializeSegment, ProducesAValidZeroedSegment) {
  const std::vector<std::byte> image = valid_image();

  const core::HeaderCheck check = validate(image);
  EXPECT_FALSE(check.error) << check.error.message();
  EXPECT_EQ(check.geometry, kGeometry);

  core::SegmentHeader header{};
  std::memcpy(&header, image.data(), sizeof header);
  EXPECT_EQ(header.magic, core::kMagic);
  EXPECT_EQ(header.wire_version, core::kWireVersion);
  EXPECT_EQ(header.layout_hash, core::layout_hash(kGeometry));
  EXPECT_EQ(header.created_unix_ns, kCreatedNs);
  EXPECT_TRUE(std::all_of(image.begin() + core::kControlOffset, image.end(),
                          [](std::byte b) { return b == std::byte{0}; }));
}

// A creator that died mid-initialisation leaves magic == 0 and arbitrary bytes behind.
TEST(InitializeSegment, ReinitialisesOverLeftoversFromACrashedCreator) {
  const Segment segment{kSize};
  std::fill(segment.span().begin(), segment.span().end(), std::byte{0xAB});
  std::memset(segment.span().data(), 0, sizeof(std::uint64_t));

  EXPECT_EQ(core::validate_header(segment.span()).error, errc::segment_incomplete);
  core::initialize_segment(segment.span(), kGeometry, kCreatedNs);

  EXPECT_FALSE(core::validate_header(segment.span()).error);
  const core::ControlBlock& control = core::control_of(segment.span().data());
  EXPECT_EQ(control.head, 0U);
  EXPECT_EQ(control.epoch, 0U);
  EXPECT_TRUE(std::all_of(segment.span().begin() + core::kSlotsOffset, segment.span().end(),
                          [](std::byte b) { return b == std::byte{0}; }));
}

TEST(ValidateHeader, TooShortForAHeaderIsIncomplete) {
  for (const std::size_t size : {std::size_t{0}, std::size_t{8}, core::kSlotsOffset - 1}) {
    std::vector<std::byte> image = valid_image();
    image.resize(size);
    EXPECT_EQ(validate(image).error, errc::segment_incomplete) << size;
  }
}

TEST(ValidateHeader, ZeroMagicIsIncompleteEvenWhenEverythingElseIsWrong) {
  std::vector<std::byte> image = valid_image();
  poke<std::uint64_t>(image, offsetof(core::SegmentHeader, magic), 0);
  poke<std::uint32_t>(image, offsetof(core::SegmentHeader, wire_version), 99);
  EXPECT_EQ(validate(image).error, errc::segment_incomplete);
}

TEST(ValidateHeader, ForeignMagicIsRejectedBeforeTheVersion) {
  std::vector<std::byte> image = valid_image();
  poke<std::uint64_t>(image, offsetof(core::SegmentHeader, magic), 0x7F454C46);
  poke<std::uint32_t>(image, offsetof(core::SegmentHeader, wire_version), 2);
  EXPECT_EQ(validate(image).error, errc::bad_magic);
}

TEST(ValidateHeader, OtherWireVersionsAreRejectedBeforeTheGeometry) {
  for (const std::uint32_t version : {0U, 2U, 0xFFFF'FFFFU}) {
    std::vector<std::byte> image = valid_image();
    poke<std::uint32_t>(image, offsetof(core::SegmentHeader, wire_version), version);
    poke<std::uint32_t>(image, offsetof(core::SegmentHeader, slot_size), 96);
    EXPECT_EQ(validate(image).error, errc::version_mismatch) << version;
  }
}

TEST(ValidateHeader, ImpossibleGeometryOrNonZeroReservedBytesAreCorrupt) {
  const auto corrupt_after = [](std::size_t offset, auto value) {
    std::vector<std::byte> image = valid_image();
    poke(image, offset, value);
    return validate(image).error == errc::segment_corrupt;
  };
  EXPECT_TRUE(corrupt_after(offsetof(core::SegmentHeader, slot_size), std::uint32_t{96}));
  EXPECT_TRUE(corrupt_after(offsetof(core::SegmentHeader, slot_size), std::uint32_t{8192}));
  EXPECT_TRUE(corrupt_after(offsetof(core::SegmentHeader, slot_count), std::uint32_t{3}));
  EXPECT_TRUE(corrupt_after(offsetof(core::SegmentHeader, slot_count), std::uint32_t{0}));
  EXPECT_TRUE(corrupt_after(offsetof(core::SegmentHeader, reserved0), std::uint32_t{1}));
  EXPECT_TRUE(corrupt_after(offsetof(core::SegmentHeader, reserved) + 23, std::uint8_t{1}));
}

TEST(ValidateHeader, FileSizeMustMatchTheGeometryExactly) {
  for (const std::size_t size : {kSize - 8, kSize - 1, kSize + 1, kSize + 64}) {
    std::vector<std::byte> image = valid_image();
    image.resize(size);
    EXPECT_EQ(validate(image).error, errc::segment_corrupt) << size;
  }
}

TEST(ValidateHeader, ConsistentGeometryWithAForeignLayoutHashIsALayoutMismatch) {
  std::vector<std::byte> image = valid_image();
  poke<std::uint64_t>(image, offsetof(core::SegmentHeader, layout_hash),
                      core::layout_hash(kGeometry) ^ 1U);
  EXPECT_EQ(validate(image).error, errc::layout_mismatch);
}

TEST(ValidateHeader, CreationTimeIsDiagnosticOnly) {
  std::vector<std::byte> image = valid_image();
  poke<std::uint64_t>(image, kCreatedOffset, 0);
  EXPECT_FALSE(validate(image).error);
}

// INV7: a hostile or corrupt segment can never make validation read out of bounds or accept it.
// Success is expected only when the size and every checked header byte equal a valid image.
TEST(ValidateHeader, Inv7SeededHostileMutationsAreRejectedSafely) {
  const std::vector<std::byte> valid = valid_image();
  std::mt19937_64 rng{0x494E'5637};  // "INV7"
  std::uniform_int_distribution<int> kind{0, 2};
  std::uniform_int_distribution<std::size_t> header_byte{0, sizeof(core::SegmentHeader) - 1};
  std::uniform_int_distribution<std::size_t> flips{1, 4};
  std::uniform_int_distribution<std::size_t> length{0, 2 * kSize};
  std::uniform_int_distribution<int> value{0, 255};

  int accepted = 0;
  for (int i = 0; i < 100'000; ++i) {
    std::vector<std::byte> image = valid;
    const int k = kind(rng);
    if (k != 1) {
      for (std::size_t n = flips(rng); n > 0; --n) {
        image[header_byte(rng)] = static_cast<std::byte>(value(rng));
      }
    }
    if (k != 0) {
      image.resize(length(rng));
    }

    const bool expect_valid =
        image.size() == kSize &&
        std::equal(image.begin(), image.begin() + kCreatedOffset, valid.begin()) &&
        std::equal(image.begin() + kCreatedOffset + 8, image.begin() + sizeof(core::SegmentHeader),
                   valid.begin() + kCreatedOffset + 8);

    const core::HeaderCheck check = validate(image);
    ASSERT_EQ(!check.error, expect_valid) << "iteration " << i << ": " << check.error.message();
    if (expect_valid) {
      ASSERT_EQ(check.geometry, kGeometry);
      ++accepted;
    }
  }
  EXPECT_GT(accepted, 0);  // the oracle's "valid" branch is exercised too
}

}  // namespace

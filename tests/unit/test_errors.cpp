#include <revenant/errors.hpp>

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>
#include <system_error>

namespace {

using revenant::errc;

constexpr std::array kAllCodes = {
    errc::invalid_name,     errc::invalid_config,     errc::channel_not_found,
    errc::publisher_active, errc::segment_incomplete, errc::bad_magic,
    errc::version_mismatch, errc::layout_mismatch,    errc::geometry_mismatch,
    errc::segment_corrupt,
};

// Values may be logged or compared across builds, so they never change meaning.
static_assert(static_cast<int>(errc::invalid_name) == 1);
static_assert(static_cast<int>(errc::segment_corrupt) == 10);
static_assert(kAllCodes.size() == 10);

TEST(Errors, CategoryIsNamedRevenant) {
  EXPECT_STREQ(revenant::error_category().name(), "revenant");
}

TEST(Errors, CategoryIsASingleton) {
  EXPECT_EQ(&revenant::error_category(), &revenant::error_category());
}

TEST(Errors, EveryCodeHasADistinctNonEmptyMessage) {
  std::set<std::string> messages;
  for (const errc e : kAllCodes) {
    const std::string message = revenant::make_error_code(e).message();
    EXPECT_FALSE(message.empty()) << static_cast<int>(e);
    EXPECT_NE(message, "unknown revenant error") << static_cast<int>(e);
    messages.insert(message);
  }
  EXPECT_EQ(messages.size(), kAllCodes.size());
}

TEST(Errors, UnknownValuesHaveAGenericMessage) {
  EXPECT_EQ(revenant::error_category().message(0), "unknown revenant error");
  EXPECT_EQ(revenant::error_category().message(999), "unknown revenant error");
}

TEST(Errors, EnumConvertsImplicitlyToErrorCode) {
  const std::error_code ec = errc::publisher_active;
  EXPECT_TRUE(ec);
  EXPECT_EQ(ec.category(), revenant::error_category());
  EXPECT_EQ(ec.value(), static_cast<int>(errc::publisher_active));
  EXPECT_EQ(ec, errc::publisher_active);
  EXPECT_NE(ec, errc::bad_magic);
}

TEST(Errors, NeverEqualsASystemCodeWithTheSameValue) {
  for (const errc e : kAllCodes) {
    const std::error_code system{static_cast<int>(e), std::system_category()};
    EXPECT_NE(std::error_code{e}, system) << static_cast<int>(e);
  }
}

TEST(Errors, MapsToPortableConditionsWhereOneExists) {
  EXPECT_EQ(std::error_code{errc::invalid_name}, std::errc::invalid_argument);
  EXPECT_EQ(std::error_code{errc::invalid_config}, std::errc::invalid_argument);
  EXPECT_EQ(std::error_code{errc::channel_not_found}, std::errc::no_such_file_or_directory);
  EXPECT_EQ(std::error_code{errc::publisher_active}, std::errc::device_or_resource_busy);
  EXPECT_EQ(std::error_code{errc::segment_incomplete}, std::errc::resource_unavailable_try_again);
}

TEST(Errors, CodesWithoutAPortableEquivalentStayInTheRevenantCategory) {
  for (const errc e : {errc::bad_magic, errc::version_mismatch, errc::layout_mismatch,
                       errc::geometry_mismatch, errc::segment_corrupt}) {
    const std::error_condition condition = std::error_code{e}.default_error_condition();
    EXPECT_EQ(condition.category(), revenant::error_category()) << static_cast<int>(e);
    EXPECT_EQ(condition.value(), static_cast<int>(e));
    EXPECT_NE(std::error_code{e}, std::errc::invalid_argument);
  }
}

TEST(Errors, SystemErrorCarriesTheCodeAndItsMessage) {
  try {
    throw std::system_error(errc::channel_not_found, "attach \"quotes\"");
  } catch (const std::system_error& e) {
    EXPECT_EQ(e.code(), errc::channel_not_found);
    EXPECT_NE(std::string{e.what()}.find("channel not found"), std::string::npos) << e.what();
  }
}

}  // namespace

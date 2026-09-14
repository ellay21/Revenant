#include <revenant/config.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <string_view>

namespace {

// In release builds this only compiles warning-free if the macro still names its operand.
int asserted_only(int value) {
  REVENANT_ASSERT(value > 0);
  return 1;
}

TEST(Config, ParameterUsedOnlyInAnAssertionIsNotUnused) {
  EXPECT_EQ(asserted_only(1), 1);
}

TEST(Config, VersionMatchesProjectVersion) {
  EXPECT_EQ(revenant::version(), std::string_view{REVENANT_TEST_EXPECTED_VERSION});
}

TEST(Config, VersionIsMajorMinorPatch) {
  const std::string_view v = revenant::version();
  ASSERT_FALSE(v.empty());
  EXPECT_EQ(std::count(v.begin(), v.end(), '.'), 2);
  EXPECT_TRUE(
      std::all_of(v.begin(), v.end(), [](char c) { return c == '.' || (c >= '0' && c <= '9'); }));
  EXPECT_NE(v.front(), '.');
  EXPECT_NE(v.back(), '.');
}

TEST(Config, CacheLineIsPowerOfTwo) {
  static_assert(std::has_single_bit(revenant::kCacheLine));
  EXPECT_GE(revenant::kCacheLine, 64U);
}

#ifdef NDEBUG

TEST(Config, AssertIsCompiledOutInRelease) {
  int evaluations = 0;
  REVENANT_ASSERT(++evaluations == 0);
  EXPECT_EQ(evaluations, 0);
}

#else

TEST(Config, PassingAssertEvaluatesConditionOnce) {
  int evaluations = 0;
  REVENANT_ASSERT(++evaluations == 1);
  EXPECT_EQ(evaluations, 1);
}

TEST(ConfigDeathTest, FailingAssertAbortsWithExpressionAndLocation) {
  EXPECT_DEATH(REVENANT_ASSERT(1 + 1 == 3),
               "assertion failed: 1 \\+ 1 == 3 \\(.*test_config\\.cpp:[0-9]+\\)");
}

#endif

}  // namespace

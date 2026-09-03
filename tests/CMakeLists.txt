include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  SYSTEM)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# One executable per suite; the suite name doubles as the ctest label.
function(revenant_add_test_suite target label)
  add_executable(${target} ${ARGN})
  target_link_libraries(${target} PRIVATE revenant::revenant GTest::gtest_main)
  revenant_set_warnings(${target})
  gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS ${label})
endfunction()

revenant_add_test_suite(revenant_unit_tests unit unit/test_config.cpp)
target_compile_definitions(revenant_unit_tests
                           PRIVATE REVENANT_TEST_EXPECTED_VERSION="${PROJECT_VERSION}")

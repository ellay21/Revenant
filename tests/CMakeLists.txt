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
find_package(Threads REQUIRED)

# One executable per suite; the suite name doubles as the ctest label.
function(revenant_add_test_suite target label)
  add_executable(${target} ${ARGN})
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${target} PRIVATE revenant::revenant GTest::gtest_main Threads::Threads)
  revenant_set_warnings(${target})
  gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST PROPERTIES LABELS ${label})
endfunction()

revenant_add_test_suite(
  revenant_unit_tests
  unit
  unit/test_atomics.cpp
  unit/test_channel_config.cpp
  unit/test_config.cpp
  unit/test_errors.cpp
  unit/test_layout.cpp
  unit/test_payload.cpp
  unit/test_ring.cpp
  unit/test_seqlock.cpp
  unit/test_validate_header.cpp)
target_compile_definitions(revenant_unit_tests
                           PRIVATE REVENANT_TEST_EXPECTED_VERSION="${PROJECT_VERSION}")

# Real /dev/shm, real file descriptors, forked children.
revenant_add_test_suite(revenant_integration_tests integration integration/test_lease.cpp
                        integration/test_publisher.cpp integration/test_shm_segment.cpp
                        integration/test_subscriber.cpp)
if(REVENANT_FAULT_INJECTION)
  target_sources(revenant_integration_tests PRIVATE integration/test_fault.cpp)
endif()

# Message count scales with REVENANT_STRESS_MESSAGES (default 10^6).
revenant_add_test_suite(revenant_stress_tests stress stress/stress_channel.cpp
                        stress/stress_seqlock.cpp)

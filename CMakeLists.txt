cmake_minimum_required(VERSION 3.25)

project(
  revenant
  VERSION 0.1.0
  DESCRIPTION "Crash-safe shared-memory market-data fan-out for Linux"
  HOMEPAGE_URL "https://github.com/ellay21/Revenant"
  LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "revenant targets Linux only: it relies on /dev/shm and OFD locks")
endif()

option(REVENANT_BUILD_TESTS "Build the test suites" ${PROJECT_IS_TOP_LEVEL})
option(REVENANT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_EXTENSIONS OFF)
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

add_library(revenant STATIC src/config.cpp src/errors.cpp src/platform/shm_segment.cpp)
add_library(revenant::revenant ALIAS revenant)
target_include_directories(revenant PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>)
target_compile_features(revenant PUBLIC cxx_std_20)
target_compile_definitions(revenant PRIVATE REVENANT_VERSION="${PROJECT_VERSION}")
revenant_set_warnings(revenant)

if(REVENANT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

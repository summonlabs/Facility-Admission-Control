# Facility Admission Control - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# DCCP Tranche 6 (Facility Policy, Tenancy and Entitlement), repository 44 of
# the canonical 72-runtime Data Center Control Plane.
#
# Everything is target based: no directory-global flags are set, so the
# library, the tool, the examples, the benchmarks and the tests each carry
# exactly the options they need and nothing this project uses for itself can
# leak into the installed package.

cmake_minimum_required(VERSION 3.20)

project(FacilityAdmissionControl
        VERSION 1.0.0
        DESCRIPTION "Generation-bound facility admission decisions for the Data Center Control Plane"
        HOMEPAGE_URL "https://github.com/summonlabs/Facility-Admission-Control"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(FAC_BUILD_TESTS "Build the Facility Admission Control test suite" ON)
option(FAC_BUILD_TOOLS "Build the facctl command line tool" ON)
option(FAC_BUILD_EXAMPLES "Build the Facility Admission Control examples" ON)
option(FAC_BUILD_BENCHMARKS "Build the Facility Admission Control benchmarks" ON)
option(FAC_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(FAC_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(FAC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/FAC"
    CACHE STRING "Install destination of the Facility Admission Control CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it becomes part of the installed package.
function(fac_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(FAC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Od>)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wold-style-cast -Wnon-virtual-dtor -Wcast-align -Wunused -Woverloaded-virtual
      -Wnull-dereference -Wdouble-promotion -Wformat=2)
    if(FAC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(FAC_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

function(fac_add_executable target)
  add_executable(${target} ${ARGN})
  fac_configure_target(${target})
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(FAC_SOURCES
  src/core/version.cpp
  src/core/error.cpp
  src/core/text.cpp
  src/core/crc32c.cpp
  src/core/sha256.cpp
  src/core/digest.cpp
  src/core/ident.cpp
  src/core/time.cpp
  src/codec/codec.cpp
  src/model/quantity.cpp
  src/model/scope.cpp
  src/model/evidence.cpp
  src/model/capacity.cpp
  src/model/tenancy.cpp
  src/model/facility_state.cpp
  src/model/request.cpp
  src/model/decision.cpp
  src/model/reservation.cpp
  src/engine/policy.cpp
  src/engine/evaluate.cpp
  src/engine/commit.cpp
  src/engine/engine.cpp
  src/ledger/ledger.cpp
  src/ledger/records.cpp
  src/durable/format.cpp
  src/durable/journal.cpp
  src/durable/lock.cpp
  src/durable/file_handle.cpp
  src/durable/store.cpp
  src/cli/cli.cpp
)

set(FAC_PUBLIC_HEADERS
  include/fac/fac.hpp
  include/fac/version.hpp
  include/fac/core/checked.hpp
  include/fac/core/crc32c.hpp
  include/fac/core/digest.hpp
  include/fac/core/error.hpp
  include/fac/core/ident.hpp
  include/fac/core/limits.hpp
  include/fac/core/sha256.hpp
  include/fac/core/text.hpp
  include/fac/core/time.hpp
  include/fac/codec/codec.hpp
  include/fac/model/capacity.hpp
  include/fac/model/decision.hpp
  include/fac/model/evidence.hpp
  include/fac/model/facility_state.hpp
  include/fac/model/quantity.hpp
  include/fac/model/request.hpp
  include/fac/model/reservation.hpp
  include/fac/model/scope.hpp
  include/fac/model/tenancy.hpp
  include/fac/engine/engine.hpp
  include/fac/engine/policy.hpp
  include/fac/ledger/ledger.hpp
  include/fac/durable/format.hpp
  include/fac/durable/journal.hpp
  include/fac/durable/lock.hpp
  include/fac/durable/store.hpp
  include/fac/cli/cli.hpp
)

add_library(facility_admission_control STATIC ${FAC_SOURCES} ${FAC_PUBLIC_HEADERS})
add_library(FAC::facility_admission_control ALIAS facility_admission_control)

target_compile_features(facility_admission_control PUBLIC cxx_std_20)
target_include_directories(facility_admission_control
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(facility_admission_control PROPERTIES
  OUTPUT_NAME facility_admission_control
  EXPORT_NAME facility_admission_control
  POSITION_INDEPENDENT_CODE ON)
fac_configure_target(facility_admission_control)

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS facility_admission_control
  EXPORT FACTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/fac DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE NOTICE README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/FAC)

install(EXPORT FACTargets
  FILE FACTargets.cmake
  NAMESPACE FAC::
  DESTINATION ${FAC_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FACConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/FACConfig.cmake
  INSTALL_DESTINATION ${FAC_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/FACConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/FACConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/FACConfigVersion.cmake
  DESTINATION ${FAC_INSTALL_CMAKEDIR})

# ---------------------------------------------------------------------------
# Tool
# ---------------------------------------------------------------------------

if(FAC_BUILD_TOOLS)
  fac_add_executable(facctl tools/facctl/main.cpp)
  target_link_libraries(facctl PRIVATE FAC::facility_admission_control)
  install(TARGETS facctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------

if(FAC_BUILD_EXAMPLES)
  foreach(example_name IN ITEMS
      admit_basic
      refuse_and_defer
      durable_lifecycle
      commit_and_evidence)
    fac_add_executable(fac_example_${example_name} examples/${example_name}.cpp)
    target_link_libraries(fac_example_${example_name} PRIVATE FAC::facility_admission_control)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------

if(FAC_BUILD_BENCHMARKS)
  fac_add_executable(fac_benchmark benchmarks/bench_admission.cpp)
  target_link_libraries(fac_benchmark PRIVATE FAC::facility_admission_control)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(FAC_BUILD_TESTS)
  enable_testing()

  add_library(fac_test_support STATIC
    tests/test_support.cpp
    tests/test_main.cpp
    tests/process_support.cpp)
  target_link_libraries(fac_test_support PUBLIC FAC::facility_admission_control)
  target_include_directories(fac_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_definitions(fac_test_support PUBLIC
    FAC_CMAKE_VERSION="${PROJECT_VERSION}"
    FAC_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    FAC_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    FAC_CLI_PATH="$<TARGET_FILE:facctl>")
  fac_configure_target(fac_test_support)

  add_executable(fac_tests
    tests/tests_core.cpp
    tests/tests_codec.cpp
    tests/tests_model.cpp
    tests/tests_engine_verdicts.cpp
    tests/tests_engine_boundary.cpp
    tests/tests_engine_staleness.cpp
    tests/tests_engine_idempotency.cpp
    tests/tests_engine_fencing.cpp
    tests/tests_ledger_replay.cpp
    tests/tests_durable.cpp
    tests/tests_multiprocess.cpp
    tests/tests_concurrency.cpp
    tests/tests_property.cpp
    tests/tests_adversarial.cpp
    tests/tests_cli.cpp)
  target_link_libraries(fac_tests PRIVATE fac_test_support)
  fac_configure_target(fac_tests)

  add_test(NAME fac.tests COMMAND fac_tests)

  # A deliberately unbounded test runner: the suite is expected to finish on
  # its own, and a hang is a defect to diagnose rather than to hide behind a
  # watchdog. No TIMEOUT property is set on any test in this project.
  add_executable(fac_crash_child tests/crash_child.cpp)
  target_link_libraries(fac_crash_child PRIVATE fac_test_support)
  fac_configure_target(fac_crash_child)

  add_test(NAME fac.crash_recovery
    COMMAND fac_tests --only-suite multiprocess_crash)
  add_test(NAME fac.installed_consumer
    COMMAND ${CMAKE_COMMAND}
      -DFAC_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DFAC_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
      -DCMAKE_BUILD_TYPE=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunConsumerCheck.cmake)
endif()

message(STATUS "Facility Admission Control ${PROJECT_VERSION}")
message(STATUS "  C++ standard       : ${CMAKE_CXX_STANDARD}")
message(STATUS "  warnings as errors : ${FAC_WARNINGS_AS_ERRORS}")
message(STATUS "  AddressSanitizer   : ${FAC_ENABLE_ASAN}")
message(STATUS "  tests              : ${FAC_BUILD_TESTS}")
message(STATUS "  tools              : ${FAC_BUILD_TOOLS}")
message(STATUS "  examples           : ${FAC_BUILD_EXAMPLES}")
message(STATUS "  benchmarks         : ${FAC_BUILD_BENCHMARKS}")

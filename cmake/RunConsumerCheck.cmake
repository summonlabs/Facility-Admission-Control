# Facility Admission Control - installed-package consumer validation.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Run as a script by CTest:
#
#   cmake -DFAC_SOURCE_DIR=... -DFAC_BINARY_DIR=... -DCMAKE_BUILD_TYPE=<config>
#         -P cmake/RunConsumerCheck.cmake
#
# It stages an install into a prefix that the build tree does not touch,
# configures the independent consumer project against that prefix only, builds
# it, and runs it. A consumer that merely links is not proof, so the program is
# executed and its exit code is required to be zero.

cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS FAC_SOURCE_DIR FAC_BINARY_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "RunConsumerCheck.cmake requires -${required}")
  endif()
endforeach()

if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
  set(CMAKE_BUILD_TYPE Release)
endif()

set(prefix "${FAC_BINARY_DIR}/consumer-prefix")
set(consumer_build "${FAC_BINARY_DIR}/consumer-build")

message(STATUS "consumer check: staging install into ${prefix}")
file(REMOVE_RECURSE "${prefix}")
file(REMOVE_RECURSE "${consumer_build}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${FAC_BINARY_DIR}" --config "${CMAKE_BUILD_TYPE}" --prefix "${prefix}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed (${install_result})\n${install_output}\n${install_error}")
endif()

set(package_config "${prefix}/lib/cmake/FAC/FACConfig.cmake")
if(NOT EXISTS "${package_config}")
  message(FATAL_ERROR "the installed package does not provide ${package_config}")
endif()

message(STATUS "consumer check: configuring the out-of-tree consumer")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${FAC_SOURCE_DIR}/tests/consumer"
          -B "${consumer_build}"
          -DCMAKE_PREFIX_PATH=${prefix}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "consumer configure failed (${configure_result})\n${configure_output}\n${configure_error}")
endif()

message(STATUS "consumer check: building the consumer")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config "${CMAKE_BUILD_TYPE}"
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed (${build_result})\n${build_output}\n${build_error}")
endif()

file(GLOB consumer_candidates
     "${consumer_build}/fac_consumer"
     "${consumer_build}/fac_consumer.exe"
     "${consumer_build}/Release/fac_consumer.exe"
     "${consumer_build}/Debug/fac_consumer.exe"
     "${consumer_build}/RelWithDebInfo/fac_consumer.exe"
     "${consumer_build}/MinSizeRel/fac_consumer.exe")
list(LENGTH consumer_candidates candidate_count)
if(candidate_count EQUAL 0)
  message(FATAL_ERROR "the consumer executable was not produced in ${consumer_build}")
endif()
list(GET consumer_candidates 0 consumer_program)

message(STATUS "consumer check: running ${consumer_program}")
execute_process(
  COMMAND "${consumer_program}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
message(STATUS "consumer output:\n${run_output}")
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "the consumer failed at runtime (${run_result})\n${run_error}")
endif()
if(NOT run_output MATCHES "consumer-ok")
  message(FATAL_ERROR "the consumer did not report success:\n${run_output}")
endif()

message(STATUS "consumer check: PASSED")

# Facility Admission Control - independent downstream consumer.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project is deliberately not part of the main build. It is configured
# against an installed prefix only, through find_package(FAC CONFIG REQUIRED),
# which is what proves that the exported package is usable by a consumer that
# shares nothing with this repository except the installed artifacts.

cmake_minimum_required(VERSION 3.20)

project(fac_consumer
        VERSION 1.0.0
        DESCRIPTION "Out-of-tree consumer of the installed Facility Admission Control package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(FAC 1.0 CONFIG REQUIRED)

add_executable(fac_consumer main.cpp)
target_link_libraries(fac_consumer PRIVATE FAC::facility_admission_control)
target_compile_features(fac_consumer PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(fac_consumer PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
  target_compile_options(fac_consumer PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The consumer reports the version it was built against, so the validation can
# prove the installed headers and the installed library belong together.
target_compile_definitions(fac_consumer PRIVATE
  FAC_CONSUMER_EXPECTED_VERSION="${FAC_VERSION}")

#
# Copyright (c) 2020 Circuit Dojo LLC
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.13.4)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --long
    WORKING_DIRECTORY                ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE                  version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE                   stderr
    RESULT_VARIABLE                  return_code
  )
  if(return_code)
    message(STATUS "git describe failed: ${stderr}; ${KERNEL_VERSION_STRING} will be used instead")
  elseif(CMAKE_VERBOSE_MAKEFILE)
    message(STATUS "git describe stderr: ${stderr}")
  endif()
endif()

if(version)
  string(REGEX REPLACE "^([0-9]+).*$" "\\1" version_major ${version})
  string(REGEX REPLACE "^[0-9]+\.([0-9]+).*$" "\\1" version_minor "${version}")
  string(REGEX REPLACE "^[0-9]+\.[0-9]+\.([0-9]+).*$" "\\1" version_patch "${version}")
  string(REGEX REPLACE "^[0-9]+\.[0-9]+\.[0-9]+-([0-9]+)-.*$" "\\1" version_commit "${version}")
  string(REGEX REPLACE "^[0-9]+\.[0-9]+\.[0-9]+-[0-9]+-(.*)$" "\\1" version_hash "${version}")

  zephyr_compile_definitions(PYRINAS_APP_VERSION_MAJOR=${version_major})
  zephyr_compile_definitions(PYRINAS_APP_VERSION_MINOR=${version_minor})
  zephyr_compile_definitions(PYRINAS_APP_VERSION_PATCH=${version_patch})
  zephyr_compile_definitions(PYRINAS_APP_VERSION_COMMIT=${version_commit})
  zephyr_compile_definitions(PYRINAS_APP_VERSION_HASH=${version_hash})

  message(STATUS "Version: ${version_major}.${version_minor}.${version_patch}-${version_commit}-${version_hash}")
endif()

# Name the project
project(pyrinas_cloud_sample)

# Get th source
FILE(GLOB app_sources src/*.c)
FILE(GLOB app_weak_sources ${PYRINAS_DIR}/lib/app/*.c)

target_sources(app PRIVATE ${app_sources} ${app_weak_sources})

# Add external Rust lib directory
set(pyrinas_codec_example_dir   ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set(pyrinas_codec_example_include_dir   ${CMAKE_CURRENT_SOURCE_DIR}/lib/include)

# Add the library
add_library(pyrinas_codec_example_lib STATIC IMPORTED GLOBAL)

# Set the paths
set_target_properties(pyrinas_codec_example_lib PROPERTIES IMPORTED_LOCATION             ${pyrinas_codec_example_dir}/libpyrinas_codec_example.a)
set_target_properties(pyrinas_codec_example_lib PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${pyrinas_codec_example_include_dir})

# Link them!
target_link_libraries(app PUBLIC pyrinas_codec_example_lib -Wl,--allow-multiple-definition)

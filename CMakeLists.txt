#
# Copyright (c) 2020 Circuit Dojo
#
# SPDX-License-Identifier: Apache-2.0
#

# Point to Pyrinas root directory.
set(PYRINAS_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE PATH "Pyrinas root directory")

zephyr_include_directories(include)

add_subdirectory(lib)
add_subdirectory(ext)
add_subdirectory(subsys)

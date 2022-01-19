#
# Copyright (c) 2022 Circuit Dojo
#
# SPDX-License-Identifier: Apache-2.0
#

# Point to Pyrinas root directory.
set(PYRINAS_DIR ${CMAKE_CURRENT_LIST_DIR} CACHE PATH "Pyrinas root directory")

if (CONFIG_PYRINAS)
zephyr_include_directories(include)

add_subdirectory(lib)
endif()
#
# Copyright (c) 2020 Circuit Dojo LLC
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(pyrinas_bridge)

target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE src/ble.c)

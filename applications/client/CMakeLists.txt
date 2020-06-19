#
# Copyright (c) 2020 Circuit Dojo LLC
#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.8.2)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(pyrinas_client)

FILE(GLOB app_sources src/*.c)
FILE(GLOB ble_lib_sources ${PYRINAS_DIR}/lib/ble/*.c)
FILE(GLOB app_weak_sources ${PYRINAS_DIR}/lib/app/*.c)

target_sources(app PRIVATE ${app_sources} ${ble_lib_sources} ${app_weak_sources})
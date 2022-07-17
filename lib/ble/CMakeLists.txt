#
# Copyright (c) 2020 Circuit Dojo LLC
#
# SPDX-License-Identifier: Apache-2.0
#

zephyr_library()
zephyr_library_sources(
  ble_m.c
)

if (CONFIG_PYRINAS_PERIPH_ENABLED)
zephyr_library_sources(ble_peripheral.c)
endif()

if (CONFIG_PYRINAS_CENTRAL_ENABLED)
zephyr_library_sources(ble_central.c)
endif()
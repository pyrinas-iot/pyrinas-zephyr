#
# Copyright (c) 2020 Circuit Dojo LLC
#
# SPDX-License-Identifier: Apache-2.0
#

if (CONFIG_PYRINAS_ENABLED)
zephyr_library()
zephyr_library_sources(
  app/app_weak.c
  ble/ble_m.c
  proto/command.pb.c
)

if (CONFIG_PYRINAS_PERIPH_ENABLED)
zephyr_library_sources(ble/ble_peripheral.c)
endif()

if (CONFIG_PYRINAS_CENTRAL_ENABLED)
zephyr_library_sources(ble/ble_central.c)
endif()

endif()

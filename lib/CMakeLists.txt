#
# Copyright (c) 2020 Circuit Dojo LLC
#
# SPDX-License-Identifier: Apache-2.0
#

if (CONFIG_PYRINAS_AT_SHELL)
add_subdirectory(at_shell)
endif()

if (CONFIG_PYRINAS_CLOUD_ENABLED)
add_subdirectory(cloud)
endif()

if (CONFIG_PYRINAS_BLUETOOTH_ENABLED)
add_subdirectory(ble)
endif()

add_subdirectory(codec)
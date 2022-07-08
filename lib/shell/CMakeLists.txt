#
# Copyright (c) 2020 Circuit Dojo LLC
#
# SPDX-License-Identifier: Apache-2.0
#

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_PYRINAS_AT_SHELL at_shell.c)
zephyr_library_sources_ifdef(CONFIG_PYRINAS_SETTINGS_SHELL settings_shell.c)
zephyr_library_sources_ifdef(CONFIG_PYRINAS_CREDENTIALS_SHELL credentials_shell.c)

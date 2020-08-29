/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <logging/log.h>
LOG_MODULE_REGISTER(app_weak);

void __weak early_setup(void)
{
  LOG_INF("early_setup");
}

void __weak setup(void)
{
  LOG_INF("setup");
}

void __weak loop(void)
{
}
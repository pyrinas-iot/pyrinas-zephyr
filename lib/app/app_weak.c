/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>

void __weak setup(void)
{
  printk("App weak!\n");
}

void __weak loop(void)
{
}
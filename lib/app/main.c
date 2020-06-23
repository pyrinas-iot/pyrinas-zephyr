/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <settings/settings.h>
#include <app/app.h>

static void settings_init(void)
{
	int err;

	err = settings_subsys_init();
	if (err)
	{
		return;
	}
}

void main(void)
{

	/* Init settings used by BT and others */
	settings_init();

	// User Setup function
	setup();

	while (1)
	{
		// User loop function
		loop();

		// Yeild so other threads can work
		k_yield();
	}
}
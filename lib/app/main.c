/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <settings/settings.h>
#include <drivers/counter.h>
#include <app/app.h>

static void settings_init(void)
{
	int err;

	err = settings_subsys_init();
	if (err)
	{
		return;
	}
/* RTC control */
struct device *rtc;

#ifdef CONFIG_PCF85063A
static void rtc_init()
{

	// Get the device
	rtc = device_get_binding("PCF85063A");
	if (rtc == NULL)
	{
		LOG_ERR("Failed to get RTC device binding\n");
		return;
	}

	LOG_INF("device is %p, name is %s", rtc, log_strdup(rtc->name));

	// 2 seconds
	const struct counter_alarm_cfg cfg = {
			.ticks = 10,
	};

	// Set the alarm
	int ret = counter_set_channel_alarm(rtc, 0, &cfg);
	if (ret)
	{
		LOG_ERR("Unable to set alarm");
	}
}
#endif

static bool timer_flag = false;

void main(void)
{

	/* Init settings used by BT and others */
	settings_init();

	// Setting up the RTC on I2C2
#ifdef CONFIG_PCF85063A
	rtc_init();
#endif

	// User Setup function
	setup();

	while (1)
	{

		if (!timer_flag)
		{
			int ret = counter_get_pending_int(rtc);
			LOG_INF("Interrupt? %d", ret);

			if (ret == 1)
			{
				timer_flag = true;

				int ret = counter_cancel_channel_alarm(rtc, 0);
				if (ret)
				{
					LOG_ERR("Unable to cancel channel alarm!");
				}
			}
		}

		// User loop function
		loop();

		// Yeild so other threads can work
		k_yield();
	}
}
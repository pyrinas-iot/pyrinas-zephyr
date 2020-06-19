/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <sys/printk.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <drivers/i2c.h>
#include <settings/settings.h>

#include <ble/ble_m.h>
#include <app/app.h>

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS 1000

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay)
#define LED0 DT_GPIO_LABEL(LED0_NODE, gpios)
#define PIN DT_GPIO_PIN(LED0_NODE, gpios)
#if DT_PHA_HAS_CELL(LED0_NODE, gpios, flags)
#define FLAGS DT_GPIO_FLAGS(LED0_NODE, gpios)
#endif
#else
/* A build error here means your board isn't set up to blink an LED. */
#error "Unsupported board: led0 devicetree alias is not defined"
#define LED0 ""
#define PIN 0
#endif
/* PCF8506 Address*/
#define PCF8506_I2C_ADDR 0x51

#ifndef FLAGS
#define FLAGS 0
#endif

struct device *blue_led;

static void buttons_leds_init(void)
{
	/*
	 * Set up LED
	 */
	int ret;

	blue_led = device_get_binding(LED0);
	__ASSERT(blue_led, "unable to get binding.");

	ret = gpio_pin_configure(blue_led, PIN, GPIO_OUTPUT_ACTIVE | FLAGS);
	__ASSERT(ret == 0, "unable to configure pin: %d", ret);

	/*
	 * Set up button
	 */
}

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

	/* Init buttons and LEDs */
	buttons_leds_init();
	settings_init();

	// User Setup function
	setup();

	while (1)
	{
		// User loop function
		loop();

		// ? Own thread?
		/* Ble Loop */
		// ble_process();

		// Yeild so other threads can work
		k_yield();
	}
}
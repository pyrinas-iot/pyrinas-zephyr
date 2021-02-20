/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app/app_includes.h>

#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(app);

/*
 * Devicetree helper macro which gets the 'flags' cell from a 'gpios'
 * property, or returns 0 if the property has no 'flags' cell.
 */

#define FLAGS_OR_ZERO(node)                          \
    COND_CODE_1(DT_PHA_HAS_CELL(node, gpios, flags), \
                (DT_GPIO_FLAGS(node, gpios)),        \
                (0))

/*
  * The led0 devicetree alias is optional. If present, we'll use it
  * to turn on the LED whenever the button is pressed.
  */

#define LED0_NODE DT_ALIAS(led0)

#if DT_NODE_HAS_STATUS(LED0_NODE, okay) && DT_NODE_HAS_PROP(LED0_NODE, gpios)
#define LED0_GPIO_LABEL DT_GPIO_LABEL(LED0_NODE, gpios)
#define LED0_GPIO_PIN DT_GPIO_PIN(LED0_NODE, gpios)
#define LED0_GPIO_FLAGS (GPIO_OUTPUT | FLAGS_OR_ZERO(LED0_NODE))
#endif

static void my_expiry_function(struct k_timer *timer);
K_TIMER_DEFINE(my_timer, my_expiry_function, NULL);

char count[10];
int tx_counter = 0;

static void my_expiry_function(struct k_timer *timer)
{
    if (ble_is_connected())
    {
        snprintf(count, sizeof(count), "%i", tx_counter++);
        ble_publish("ping", count);
        LOG_INF("sending: \"%s\" \"%s\"", "ping", log_strdup(count));
    }

    k_timer_start(&my_timer, K_MSEC(500), K_NO_WAIT);
}

void evt_cb(char *name, char *data)
{
    LOG_INF("\"%s\" \"%s\"", log_strdup(name), log_strdup(data));
}

static struct device *led;

void led_init(void)
{
    int ret;

    led = device_get_binding(LED0_GPIO_LABEL);
    if (led == NULL)
    {
        printk("Didn't find LED device %s\n", LED0_GPIO_LABEL);
        return;
    }

    ret = gpio_pin_configure(led, LED0_GPIO_PIN, LED0_GPIO_FLAGS);
    if (ret != 0)
    {
        printk("Error %d: failed to configure LED device %s pin %d\n",
               ret, LED0_GPIO_LABEL, LED0_GPIO_PIN);
        return;
    }
}

void setup(void)
{
    // Message!
    LOG_INF("Start of Pyrinas Hub example!");

    /* LED init */
    led_init();

    // Default config for central mode
    BLE_STACK_CENTRAL_DEF(init);

    /* BLE initialization */
    ble_stack_init(&init);

    // Subscribe
    ble_subscribe("pong", evt_cb);

    // Start message timer
    k_timer_start(&my_timer, K_SECONDS(1), K_NO_WAIT);
}

void loop(void)
{
    // Toggle LED
    gpio_pin_toggle(led, LED0_GPIO_PIN);
    k_msleep(500);
}
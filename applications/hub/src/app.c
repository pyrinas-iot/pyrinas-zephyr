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

#define FLAGS_OR_ZERO(node)                          \
    COND_CODE_1(DT_PHA_HAS_CELL(node, gpios, flags), \
                (DT_GPIO_FLAGS(node, gpios)),        \
                (0))

#define LED0_NODE DT_ALIAS(led0)
#define SW0_NODE DT_ALIAS(sw0)

#define LED0_GPIO_LABEL DT_GPIO_LABEL(LED0_NODE, gpios)
#define LED0_GPIO_PIN DT_GPIO_PIN(LED0_NODE, gpios)
#define LED0_GPIO_FLAGS (GPIO_OUTPUT | FLAGS_OR_ZERO(LED0_NODE))

#define SW0_GPIO_LABEL DT_GPIO_LABEL(SW0_NODE, gpios)
#define SW0_GPIO_PIN DT_GPIO_PIN(SW0_NODE, gpios)
#define SW0_GPIO_FLAGS (GPIO_INPUT | FLAGS_OR_ZERO(SW0_NODE))

static void my_expiry_function(struct k_timer *timer);
K_TIMER_DEFINE(my_timer, my_expiry_function, NULL);

static struct gpio_callback button_cb_data;

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

static const struct device *led;

void led_init(void)
{
    int ret;

    led = device_get_binding(LED0_GPIO_LABEL);
    if (led == NULL)
    {
        LOG_ERR("Didn't find LED device %s\n", LED0_GPIO_LABEL);
        return;
    }

    ret = gpio_pin_configure(led, LED0_GPIO_PIN, LED0_GPIO_FLAGS);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure LED device %s pin %d\n",
                ret, LED0_GPIO_LABEL, LED0_GPIO_PIN);
        return;
    }
}

void button_pressed(const struct device *dev, struct gpio_callback *cb,
                    uint32_t pins)
{
    LOG_DBG("Button pressed at %" PRIu32 "\n", k_cycle_get_32());

    ble_erase_bonds();
}

static void button_init()
{

    const struct device *button;
    int ret;

    button = device_get_binding(SW0_GPIO_LABEL);
    if (button == NULL)
    {
        LOG_ERR("Error: didn't find %s device\n", SW0_GPIO_LABEL);
        return;
    }

    ret = gpio_pin_configure(button, SW0_GPIO_PIN, SW0_GPIO_FLAGS);
    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure %s pin %d\n",
                ret, SW0_GPIO_LABEL, SW0_GPIO_PIN);
        return;
    }

    ret = gpio_pin_interrupt_configure(button,
                                       SW0_GPIO_PIN,
                                       GPIO_INT_EDGE_TO_ACTIVE);
    if (ret != 0)
    {
        printk("Error %d: failed to configure interrupt on %s pin %d\n",
               ret, SW0_GPIO_LABEL, SW0_GPIO_PIN);
        return;
    }

    gpio_init_callback(&button_cb_data, button_pressed, BIT(SW0_GPIO_PIN));
    gpio_add_callback(button, &button_cb_data);
}

static void raw_evt_handler(pyrinas_event_t *evt)
{
    LOG_INF("evt name: %s", log_strdup(evt->name.bytes));
}

void setup(void)
{
    // Message!
    LOG_INF("Start of Pyrinas Hub example!");

    /* LED init */
    led_init();

    /* Button init */
    button_init();

    // Default config for central mode
    BLE_STACK_CENTRAL_DEF(init);

    /* BLE initialization */
    ble_stack_init(&init);

    // Subscribe
    ble_subscribe("pong", evt_cb);

    /* Raw evt handler */
    ble_subscribe_raw(raw_evt_handler);

    // Start message timer
    k_timer_start(&my_timer, K_SECONDS(1), K_NO_WAIT);
}

void loop(void)
{
    // Toggle LED
    gpio_pin_toggle(led, LED0_GPIO_PIN);
    k_msleep(500);
}
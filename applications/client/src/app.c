/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <app/app_includes.h>
#include <device.h>
#include <drivers/gpio.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(app);

/*
 * Devicetree helper macro which gets the 'flags' cell from a 'gpios'
 * property, or returns 0 if the property has no 'flags' cell.
 */

#define FLAGS_OR_ZERO(node)                        \
  COND_CODE_1(DT_PHA_HAS_CELL(node, gpios, flags), \
              (DT_GPIO_FLAGS(node, gpios)),        \
              (0))

 /*
  * Get button configuration from the devicetree sw0 alias.
  *
  * At least a GPIO device and pin number must be provided. The 'flags'
  * cell is optional.
  */

#define SW0_NODE DT_ALIAS(sw0)

#if DT_NODE_HAS_STATUS(SW0_NODE, okay)
#define SW0_GPIO_LABEL DT_GPIO_LABEL(SW0_NODE, gpios)
#define SW0_GPIO_PIN DT_GPIO_PIN(SW0_NODE, gpios)
#define SW0_GPIO_FLAGS (GPIO_INPUT | FLAGS_OR_ZERO(SW0_NODE))
#else
#error "Unsupported board: sw0 devicetree alias is not defined"
#define SW0_GPIO_LABEL ""
#define SW0_GPIO_PIN 0
#define SW0_GPIO_FLAGS 0
#endif

static struct gpio_callback button_cb_data;
struct device *button;

static void my_expiry_function(struct k_timer *timer);
K_TIMER_DEFINE(my_timer, my_expiry_function, NULL);

char count[10];
int tx_counter = 0;

static void my_expiry_function(struct k_timer *timer)
{
    if (ble_is_connected())
    {
        snprintf(count, sizeof(count), "%i", tx_counter++);
        ble_publish("pong", count);
        LOG_INF("sending: \"%s\" \"%s\"", "pong", log_strdup(count));
    }

    k_timer_start(&my_timer, K_MSEC(500), K_NO_WAIT);
}

void evt_cb(char *name, char *data)
{
    LOG_INF("\"%s\" \"%s\"", log_strdup(name), log_strdup(data));
}

void button_pressed(struct device *dev, struct gpio_callback *cb,
    uint32_t pins)
{
    printk("Button pressed at %" PRIu32 "\n", k_cycle_get_32());
    // ble_erase_bonds();
}

static void config_button(void)
{
    int ret;

    button = device_get_binding(SW0_GPIO_LABEL);
    if (button == NULL)
    {
        printk("Error: didn't find %s device\n", SW0_GPIO_LABEL);
        return;
    }

    ret = gpio_pin_configure(button, SW0_GPIO_PIN, SW0_GPIO_FLAGS);
    if (ret != 0)
    {
        printk("Error %d: failed to configure %s pin %d\n",
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
    printk("Set up button at %s pin %d\n", SW0_GPIO_LABEL, SW0_GPIO_PIN);
}

void setup(void)
{
    // Message!
    LOG_INF("Start of Pyrinas Client example!");

    // Config button
    config_button();

    // Default config for central mode
    BLE_STACK_PERIPH_DEF(init);

    /* BLE initialization */
    ble_stack_init(&init);

    // Subscribe to event
    ble_subscribe("ping", evt_cb);

    // Start message timer
    k_timer_start(&my_timer, K_SECONDS(1), K_NO_WAIT);
}

void loop(void)
{
    k_msleep(100);
}
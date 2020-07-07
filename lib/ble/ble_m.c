/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <settings/settings.h>

#include <bluetooth/bluetooth.h>

#include <ble/ble_m.h>
#include <ble/ble_central.h>
#include <ble/ble_peripheral.h>
#include <ble/ble_settings.h>

#include <proto/command.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(ble_m);

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

#define LED2_NODE DT_ALIAS(led2)

#if DT_NODE_HAS_STATUS(LED2_NODE, okay) && DT_NODE_HAS_PROP(LED2_NODE, gpios)
#define LED2_GPIO_LABEL DT_GPIO_LABEL(LED2_NODE, gpios)
#define LED2_GPIO_PIN DT_GPIO_PIN(LED2_NODE, gpios)
#define LED2_GPIO_FLAGS (GPIO_OUTPUT | FLAGS_OR_ZERO(LED2_NODE))
#endif

#define member_size(type, member) sizeof(((type *)0)->member)

// Queue definition
K_MSGQ_DEFINE(m_event_queue, BLE_INCOMING_PROTOBUF_SIZE, 20, BLE_QUEUE_ALIGN);

static ble_subscription_list_t m_subscribe_list; /**< Use for adding/removing subscriptions */
static ble_stack_init_t m_config;                /**< Init config */
static raw_susbcribe_handler_t m_raw_handler_ext;
static bool m_init_complete = false;

/* Related work handler for rx ring buf*/
static void bt_send_work_handler(struct k_work *work);
static K_WORK_DEFINE(bt_send_work, bt_send_work_handler);

static int subscriber_search(protobuf_event_t_name_t *event_name); // Forward declaration of subscriber_search

// Temporary evt
static protobuf_event_t evt;

/* LED for indicating status */
static struct device *led;

#ifdef LED2_GPIO_LABEL
/* Timer for flashing LED*/
static void led_flash_handler(struct k_timer *timer);
K_TIMER_DEFINE(led_flash_timer, led_flash_handler, NULL);

static void led_flash_handler(struct k_timer *timer)
{

    if (!ble_is_connected())
    {
        // Toggle this guy
        gpio_pin_toggle(led, LED2_GPIO_PIN);
        // Restart the timer
        k_timer_start(&led_flash_timer, K_SECONDS(1), K_NO_WAIT);
    }
    else
    {
        // Toggle this guy
        gpio_pin_set(led, LED2_GPIO_PIN, 1);
    }
}
#endif

static void bt_send_work_handler(struct k_work *work)
{

    while (k_msgq_num_used_get(&m_event_queue))
    {

        // Get it from the queue
        int err = k_msgq_get(&m_event_queue, &evt, K_NO_WAIT);
        if (err == 0)
        {
            // Forward to raw handler if it exists
            if (m_raw_handler_ext != NULL)
            {
                m_raw_handler_ext(&evt);
            }

            // Check if exists
            int index = subscriber_search(&evt.name);

            // If index is >= 0, we have an entry
            if (index != -1)
            {
                // Push to susbscription context
                m_subscribe_list.subscribers[index].evt_handler((char *)evt.name.bytes, (char *)evt.data.bytes);
            }
        }
    }
}

bool ble_is_connected(void)
{

    bool is_connected = false;

#if defined(CONFIG_PYRINAS_PERIPH_ENABLED)
    is_connected = ble_peripheral_is_connected();
#elif defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
    is_connected = ble_central_is_connected();
#endif
    // LOG_INF("%sconnected. %d", is_connected ? "" : "not ", m_config.mode);

    return is_connected;
}

void ble_disconnect(void)
{
#if defined(CONFIG_PYRINAS_PERIPH_ENABLED)
    ble_peripheral_disconnect();
#elif defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
    ble_central_disconnect();
#endif
}

void ble_publish(char *name, char *data)
{

    uint8_t name_length = strlen(name) + 1;
    uint8_t data_length = strlen(data) + 1;

    // Check size
    if (name_length >= member_size(protobuf_event_t_name_t, bytes))
    {
        LOG_ERR("Name must be <= %d characters.", member_size(protobuf_event_t_name_t, bytes));
        return;
    }

    // Check size
    if (data_length >= member_size(protobuf_event_t_data_t, bytes))
    {
        LOG_ERR("Data must be <= %d characters.", member_size(protobuf_event_t_data_t, bytes));
        return;
    }

    // Create an event.
    protobuf_event_t event = {
        .name.size = name_length,
        .data.size = data_length,
    };

    // Copy contents of message over
    memcpy(event.name.bytes, name, name_length);
    memcpy(event.data.bytes, data, data_length);

    // Then publish it as a raw format.
    ble_publish_raw(event);
}

void ble_publish_raw(protobuf_event_t event)
{

    // LOG_INF("publish raw: %s %s %d", log_strdup(event.name.bytes), log_strdup(event.data.bytes), m_config.mode);

    // TODO: Get address of this device
    // Copy over the address information
    // memcpy(event.faddr, gap_addr.addr, sizeof(event.faddr));

    // Encode value
    pb_byte_t output[protobuf_event_t_size];

    // Output buffer
    pb_ostream_t ostream = pb_ostream_from_buffer(output, sizeof(output));

    if (!pb_encode(&ostream, protobuf_event_t_fields, &event))
    {
        LOG_ERR("Unable to encode: %s", log_strdup(PB_GET_ERROR(&ostream)));
        return;
    }

    // TODO: send to connected device(s)
#if defined(CONFIG_PYRINAS_PERIPH_ENABLED)
    ble_peripheral_write(output, ostream.bytes_written);
#elif defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
    ble_central_write(output, ostream.bytes_written);
#endif
}

void ble_subscribe(char *name, susbcribe_handler_t handler)
{

    uint8_t name_length = strlen(name) + 1;

    // Check size
    if (name_length > member_size(protobuf_event_t_name_t, bytes))
    {
        LOG_WRN("Name must be <= %d characters.", member_size(protobuf_event_t_name_t, bytes));
        return;
    }

    // Check subscription amount
    if (m_subscribe_list.count >= BLE_SETTINGS_MAX_SUBSCRIPTIONS)
    {
        LOG_WRN("Too many subscriptions.");
        return;
    }

    ble_subscription_handler_t subscriber = {
        .evt_handler = handler};

    // Copy over info to structure.
    subscriber.name.size = name_length;
    memcpy(subscriber.name.bytes, name, name_length);

    // Check if exists
    int index = subscriber_search(&subscriber.name);

    // If index is >= 0, we have an entry
    if (index != -1)
    {
        m_subscribe_list.subscribers[index] = subscriber;
    }
    // Otherwise create a new one
    else
    {
        m_subscribe_list.subscribers[m_subscribe_list.count] = subscriber;
        m_subscribe_list.count++;
    }
}

void advertising_start(void)
{

#if defined(CONFIG_PYRINAS_PERIPH_ENABLED)
    ble_peripheral_advertising_start();
#endif
}

void scan_start(void)
{
#if defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
    ble_central_scan_start();
#endif
}

/**@brief Function for queuing events so they can read in main context.
 */
static void ble_evt_handler(const char *data, uint16_t len)
{

    // If data is valid and len > 0
    if (len && data)
    {
        // Setitng up protocol buffer data
        protobuf_event_t evt;

        // Read in buffer
        pb_istream_t istream = pb_istream_from_buffer((pb_byte_t *)data, len);

        if (!pb_decode(&istream, protobuf_event_t_fields, &evt))
        {
            LOG_ERR("Unable to decode: %s", log_strdup(PB_GET_ERROR(&istream)));
            return;
        }

        // There is a *slight* mismatch in size. So for good measure use a buffer the same size..
        uint8_t buf[BLE_INCOMING_PROTOBUF_SIZE];
        memcpy(buf, &evt, sizeof(evt));

        // static uint32_t counter = 0;

        // LOG_INF("%d \"%s\" \"%s\"", counter++, log_strdup(evt.name.bytes), log_strdup(evt.data.bytes));

        // LOG_INF("%d %d", sizeof(protobuf_event_t), BLE_INCOMING_PROTOBUF_SIZE);

        // Queue events
        // TODO: Handling overflows..
        int err = k_msgq_put(&m_event_queue, &buf, K_NO_WAIT);
        if (err)
        {
            LOG_ERR("Unable to add item to queue!");
        }

        // Start work if it hasn't been already
        k_work_submit(&bt_send_work);
    }
    else
    {
        LOG_WRN("Invalid data received!");
    }
}

// TODO: re-up this funciton
// static void radio_switch_init()
// {

//     nrf_gpio_cfg_output(VCTL1);
//     nrf_gpio_cfg_output(VCTL2);

//     // VCTL2 low, Output 2
//     // VCTL1 low, Output 1
//     nrf_gpio_pin_clear(VCTL2);
//     nrf_gpio_pin_set(VCTL1);
// }

static void ble_ready(int err)
{
    // Check for errors
    if (err)
    {
        LOG_ERR("BLE Stack init error!");
        return;
    }
    else
    {
        LOG_INF("BLE Stack Ready!");
    }

    // Load settings..
    if (IS_ENABLED(CONFIG_SETTINGS))
    {
        // Get the settings..
        int ret = settings_load();
        if (ret)
        {
            LOG_ERR("Unable to load settings.");
            return;
        }
    }

    // Init complete
    m_init_complete = true;

    // Call the ready functions for peripheral and central
#if defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
    ble_central_ready();
#elif defined(CONFIG_PYRINAS_PERIPH_ENABLED)
    ble_peripheral_ready();
#endif
}

#ifdef LED2_GPIO_LABEL
static struct device *initialize_led(void)
{
    struct device *led;
    int ret;

    led = device_get_binding(LED2_GPIO_LABEL);
    if (led == NULL)
    {
        printk("Didn't find LED device %s\n", LED2_GPIO_LABEL);
        return NULL;
    }

    ret = gpio_pin_configure(led, LED2_GPIO_PIN, LED2_GPIO_FLAGS);
    if (ret != 0)
    {
        printk("Error %d: failed to configure LED device %s pin %d\n",
               ret, LED2_GPIO_LABEL, LED2_GPIO_PIN);
        return NULL;
    }

    printk("Set up LED at %s pin %d\n", LED2_GPIO_LABEL, LED2_GPIO_PIN);

    return led;
}
#else
static struct device *initialize_led(void)
{
    LOG_WRN("led2 is not defined.");
    return NULL;
}
#endif

// TODO: transmit power
void ble_stack_init(ble_stack_init_t *p_init)
{
    int err;

    // Throw an error if NULL
    if (p_init == NULL)
    {
        __ASSERT(p_init, "Error: Invalid param.\n");
    }

    LOG_INF("Buffer item size: %d", BLE_QUEUE_ITEM_SIZE);

    // Copy over configuration
    memcpy(&m_config, p_init, sizeof(m_config));

    // Initialize connection LED
    led = initialize_led();

#ifdef LED2_GPIO_LABEL
    // Start message timer
    k_timer_start(&led_flash_timer, K_SECONDS(1), K_NO_WAIT);
#endif

// Get the port involved
#if CONFIG_BOARD_CIRCUITDOJO_FEATHER_NRF9160_NS && defined(CONFIG_HCI_NCP_RST_PORT) && defined(CONFIG_HCI_NCP_RST_PIN)
    struct device *port;
    port = device_get_binding(CONFIG_HCI_NCP_RST_PORT);
    __ASSERT(port, "Error: Bad port for boot HCI reset.\n");

    err = gpio_pin_configure(port, CONFIG_HCI_NCP_RST_PIN, GPIO_OUTPUT_INACTIVE);
    __ASSERT(err == 0, "Error: Unable to configure pin: %d", err);
#endif

    LOG_INF("Initializing Bluetooth..");
    err = bt_enable(ble_ready);
    __ASSERT(err == 0, "Error: Bluetooth init failed (err %d)\n", err);

// Delay so both IC's are in sync
#if CONFIG_BOARD_CIRCUITDOJO_FEATHER_NRF9160_NS && defined(CONFIG_HCI_NCP_RST_PORT) && defined(CONFIG_HCI_NCP_RST_PIN)
    k_msleep(1000);

    // Release
    err = gpio_pin_configure(port, CONFIG_HCI_NCP_RST_PIN, GPIO_DISCONNECTED);
    __ASSERT(err == 0, "Error: Unable to configure pin: %d", err);
#endif

#if defined(CONFIG_PYRINAS_PERIPH_ENABLED)
    // Attach handler
    ble_peripheral_attach_handler(ble_evt_handler);

    // Init peripheral mode
    ble_peripheral_init();
#elif defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
    // First, attach handler
    ble_central_attach_handler(ble_evt_handler);

    // Initialize
    ble_central_init(&m_config.central_config);
#else
#error CONFIG_PYRINAS_PERIPH_ENABLED or CONFIG_PYRINAS_CENTRAL_ENABLED must be defined.
#endif
}

// Passthrough function for subscribing to RAW events
void ble_subscribe_raw(raw_susbcribe_handler_t handler)
{
    m_raw_handler_ext = handler;
}

// deque messages, fire off the appropriate handlers
void ble_process()
{

    // Return if this module is not initalized
    if (!m_init_complete)
        return;

// Process any central tasks
#if defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
    ble_central_process();
#endif
}

// TODO: more optimized way of doing this?
static int subscriber_search(protobuf_event_t_name_t *event_name)
{

    int index = 0;

    for (; index < m_subscribe_list.count; index++)
    {
        protobuf_event_t_name_t *name = &m_subscribe_list.subscribers[index].name;

        if (name->size == event_name->size)
        {
            if (memcmp(name->bytes, event_name->bytes, name->size) == 0)
            {
                return index;
            }
        }
    }

    return -1;
}

// TODO: Deleting devices from Whitelist

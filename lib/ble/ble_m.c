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

#include <pyrinas_codec.h>

#include <ble/ble_m.h>
#include <ble/ble_central.h>
#include <ble/ble_peripheral.h>
#include <ble/ble_settings.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(ble_m);

#define member_size(type, member) sizeof(((type *)0)->member)

// Queue definition
K_MSGQ_DEFINE(m_event_rx_queue, sizeof(pyrinas_event_t), 24, BLE_QUEUE_ALIGN);
K_MSGQ_DEFINE(m_event_tx_queue, sizeof(pyrinas_event_t), 24, BLE_QUEUE_ALIGN);

static ble_subscription_list_t m_subscribe_list; /**< Use for adding/removing subscriptions */
static ble_stack_init_t m_config;                /**< Init config */
static raw_susbcribe_handler_t m_raw_handler_ext;
static bool m_init_complete = false;

/* Related work handler for rx ring buf*/
static struct k_work bt_rx_work, bt_tx_work;
static struct k_work bt_ready_work;
static struct k_work bt_erase_bonds_work;
static void bt_rx_work_handler(struct k_work *work);
static void bt_tx_work_handler(struct k_work *work);
static void bt_send_ready_work_handler(struct k_work *work);
static void bt_erase_bonds_work_handler(struct k_work *work);

static int subscriber_search(pyrinas_event_name_data_t *event_name); /* Forward declaration of subscriber_search */

/* Temporary evt */
static pyrinas_event_t rx_evt, tx_evt;

static void bt_erase_bonds_work_handler(struct k_work *work)
{
    LOG_INF("Erasing bonds..");

    int err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
    if (err)
    {
        LOG_ERR("bt_unpair: %d", err);
    }
}

static void bt_tx_work_handler(struct k_work *work)
{
    while (k_msgq_num_used_get(&m_event_tx_queue))
    {

        // Get it from the queue
        int err = k_msgq_get(&m_event_tx_queue, &tx_evt, K_NO_WAIT);
        if (err == 0)
        {

            uint8_t buf[pyrinas_event_t_size];

            size_t size = 0;

            /* Encode into something useful */
            int err = pyrinas_codec_encode(&tx_evt, buf, pyrinas_event_t_size, &size);
            if (err)
            {
                LOG_ERR("Unable to encode pyrinas message");
                return;
            }

// TODO: send to connected device(s)
#if defined(CONFIG_PYRINAS_PERIPH_ENABLED)
            ble_peripheral_write(buf, size);
#elif defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
            ble_central_write(buf, size);
#endif
        }
    }
}

static void bt_rx_work_handler(struct k_work *work)
{

    while (k_msgq_num_used_get(&m_event_rx_queue))
    {

        // Get it from the queue
        int err = k_msgq_get(&m_event_rx_queue, &rx_evt, K_NO_WAIT);
        if (err == 0)
        {
            // Forward to raw handler if it exists
            if (m_raw_handler_ext != NULL)
            {
                m_raw_handler_ext(&rx_evt);
            }

            // Check if exists
            int index = subscriber_search(&rx_evt.name);

            // If index is >= 0, we have an entry
            if (index != -1)
            {
                // Push to susbscription context
                m_subscribe_list.subscribers[index].evt_handler((char *)rx_evt.name.bytes, (char *)rx_evt.data.bytes);
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

    int err;
    uint8_t name_length = strlen(name) + 1;
    uint8_t data_length = strlen(data) + 1;

    // Check size
    if (name_length >= member_size(pyrinas_event_name_data_t, bytes))
    {
        LOG_ERR("Name must be <= %d characters.", member_size(pyrinas_event_name_data_t, bytes));
        return;
    }

    // Check size
    if (data_length >= member_size(pyrinas_event_data_t, bytes))
    {
        LOG_ERR("Data must be <= %d characters.", member_size(pyrinas_event_data_t, bytes));
        return;
    }

    // Create an event.
    pyrinas_event_t event = {
        .name.size = name_length,
        .data.size = data_length,
    };

    // Copy contents of message over
    memcpy(event.name.bytes, name, name_length);
    memcpy(event.data.bytes, data, data_length);

    err = k_msgq_put(&m_event_tx_queue, &event, K_NO_WAIT);
    if (err)
    {
        LOG_ERR("Unable to add tx event from device to queue!");
    }

    // Start work if it hasn't been already
    k_work_submit(&bt_tx_work);
}

void ble_publish_raw(pyrinas_event_t event)
{

    // LOG_INF("publish raw: %s %s %d", log_strdup(event.name.bytes), log_strdup(event.data.bytes), m_config.mode);

    // TODO: Get address of this device
    // Copy over the address information
    // memcpy(event.faddr, gap_addr.addr, sizeof(event.faddr));

    int err = k_msgq_put(&m_event_tx_queue, &event, K_NO_WAIT);
    if (err)
    {
        LOG_ERR("Unable to add tx event from device to queue!");
    }

    // Start work if it hasn't been already
    k_work_submit(&bt_tx_work);
}

void ble_subscribe(char *name, susbcribe_handler_t handler)
{

    uint8_t name_length = strlen(name) + 1;

    // Check size
    if (name_length > member_size(pyrinas_event_name_data_t, bytes))
    {
        LOG_WRN("Name must be <= %d characters.", member_size(pyrinas_event_name_data_t, bytes));
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

        /* Decode */
        pyrinas_event_t incoming;
        int err = pyrinas_codec_decode(&incoming, data, len);
        if (err)
        {
            LOG_ERR("Unable to decode incoming data.");
            return;
        }

        // LOG_DBG("evt: \"%s\"", log_strdup(evt.name.bytes));
        // LOG_INF("%d %d", sizeof(pyrinas_event_t), BLE_INCOMING_PROTOBUF_SIZE);

        // Queue events
        err = k_msgq_put(&m_event_rx_queue, &incoming, K_NO_WAIT);
        if (err)
        {
            LOG_ERR("Unable to add event from device to queue!");
        }

        // Start work if it hasn't been already
        k_work_submit(&bt_rx_work);
    }
    else
    {
        LOG_WRN("Invalid data received!");
    }
}

static void bt_send_ready_work_handler(struct k_work *work)
{

    // Load settings..
    if (IS_ENABLED(CONFIG_BT_SETTINGS))
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
        k_work_submit(&bt_ready_work);
    }
}

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

    /* Init work */
    k_work_init(&bt_rx_work, bt_rx_work_handler);
    k_work_init(&bt_tx_work, bt_tx_work_handler);
    k_work_init(&bt_ready_work, bt_send_ready_work_handler);
    k_work_init(&bt_erase_bonds_work, bt_erase_bonds_work_handler);

    // Copy over configuration
    memcpy(&m_config, p_init, sizeof(m_config));

    LOG_INF("Initializing Bluetooth..");
    err = bt_enable(ble_ready);
    __ASSERT(err == 0, "Error: Bluetooth init failed (err %d)\n", err);

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

// TODO: more optimized way of doing this?
static int subscriber_search(pyrinas_event_name_data_t *event_name)
{

    int index = 0;

    for (; index < m_subscribe_list.count; index++)
    {
        pyrinas_event_name_data_t *name = &m_subscribe_list.subscribers[index].name;

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

/* Deleting devices from Whitelist */
void ble_erase_bonds(void)
{
    k_work_submit(&bt_erase_bonds_work);
}
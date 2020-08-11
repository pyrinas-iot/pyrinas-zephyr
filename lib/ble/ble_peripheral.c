/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if CONFIG_PYRINAS_PERIPH_ENABLED

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/printk.h>
#include <sys/byteorder.h>
#include <sys/ring_buffer.h>
#include <zephyr.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/gatt.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_vs.h>
#include <bluetooth/services/nus.h>

#include <mgmt/smp_bt.h>

#include <ble/ble_handlers.h>
#include <ble/ble_settings.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(ble_peripheral);

#define BLE_TX_BUF_SIZE 2048

/* Used to track connection */
static struct bt_conn *current_conn;
static struct bt_gatt_exchange_params exchange_params;

atomic_t m_ready;

/* Maintaining advertising data*/
struct bt_le_ext_adv *adv = NULL;

/* Raw event handler */
static encoded_data_handler_t m_evt_cb = NULL;
static uint32_t nus_max_send_len;

/* Network buffer */
K_MSGQ_DEFINE(m_peripheral_event_queue, sizeof(ble_fifo_data_t), 20, BLE_QUEUE_ALIGN);

/* Advertising data */
static const struct bt_data ad[] ={
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, (sizeof(CONFIG_BT_DEVICE_NAME) - 1)),
};

static void bt_send_work_handler(struct k_work *work);
static K_WORK_DEFINE(bt_send_work, bt_send_work_handler);

/**@brief Function for starting advertising.
 */
void ble_peripheral_advertising_start(void)
{

    LOG_INF("Bluetooth initialized");

    int err;

    // If NULL set it up!
    if (adv == NULL)
    {

        // TODO: Advertising parameters. Most important being `BT_LE_ADV_OPT_CODED` and `BT_LE_ADV_OPT_EXT_ADV`
        struct bt_le_adv_param *adv_param =
            BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONNECTABLE | BT_LE_ADV_OPT_CODED | BT_LE_ADV_OPT_EXT_ADV,
                BT_GAP_ADV_FAST_INT_MIN_2,
                BT_GAP_ADV_FAST_INT_MAX_2,
                NULL);

        LOG_DBG("bt_le_ext_adv_create");
        err = bt_le_ext_adv_create(adv_param, NULL, &adv);
        if (err)
        {
            LOG_WRN("Failed to create ext adv (err %d)\n", err);
            return;
        }

        // Set the advertising data
        err = bt_le_ext_adv_set_data(adv, ad, ARRAY_SIZE(ad),
            NULL, 0);
        if (err)
        {
            LOG_ERR("Unable to set advertising data (err %d)\n", err);
            return;
        }
    }

    // Start advertising with the above parameters
    err = bt_le_ext_adv_start(adv, NULL);
    if (err)
    {
        LOG_ERR("Advertising failed to start (err %d)\n", err);
        return;
    }

    LOG_INF("Advertising successfully started");
}

static void exchange_func(struct bt_conn *conn, uint8_t err,
    struct bt_gatt_exchange_params *params)
{
    if (!err)
    {
        nus_max_send_len = bt_gatt_nus_max_send(conn);
        LOG_INF("Max MTU %d", nus_max_send_len);
    }
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    if (err)
    {
        LOG_ERR("Connection failed (err 0x%02x)\n", err);
        // TODO:if error disconnect
    }
    else
    {
        current_conn = bt_conn_ref(conn);
        exchange_params.func = exchange_func;

        err = bt_gatt_exchange_mtu(current_conn, &exchange_params);
        if (err)
        {
            LOG_WRN("bt_gatt_exchange_mtu: %d", err);
        }

        LOG_INF("Connected");
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected (reason 0x%02x)\n", reason);

    if (current_conn)
    {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    // Remove data from queue
    k_msgq_purge(&m_peripheral_event_queue);

    // Set as not ready
    atomic_set(&m_ready, 0);

    // Start advertising again
    ble_peripheral_advertising_start();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
    enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err)
    {
        LOG_INF("Security changed: %s level %u", log_strdup(addr), level);

        // Encryption is a go!
        atomic_set(&m_ready, 1);
    }
    else
    {
        LOG_ERR("Security failed: %s level %u err %d", log_strdup(addr), level,
            err);

        // Disconnect on security failure
        int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        if (err && (err != -ENOTCONN))
        {
            LOG_ERR("Cannot disconnect peer (err:%d)", err);
        }
    }
}

static struct bt_conn_cb conn_callbacks ={
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
};

static void bt_send_work_handler(struct k_work *work)
{
    bool notif_disabled = false;
    int err;

    // Check for invalid connection
    if (current_conn == NULL)
    {
        LOG_WRN("Connected not valid");
        return;
    }

    // Static ble_payload
    static ble_fifo_data_t ble_payload;

    // Get it
    err = k_msgq_get(&m_peripheral_event_queue, &ble_payload, K_NO_WAIT);
    if (err)
    {
        LOG_WRN("Unable to get data from queue");
        return;
    }

    // Send data
    err = bt_gatt_nus_send(current_conn, ble_payload.data, ble_payload.len);
    // Check if notifications are off
    if (err == -EINVAL)
    {
        LOG_WRN("Not subscribed!");
        notif_disabled = true;
    }
    else if (err) // Otherwise check if overall error
    {
        LOG_WRN("Error sending nus data. (err %d)", err);
    }

}

static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    printk("Pairing cancelled: %s\n", addr);
}

static void pairing_confirm(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    bt_conn_auth_pairing_confirm(conn);

    printk("Pairing confirmed: %s\n", addr);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_ERR("Pairing completed: %s, bonded: %d", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    LOG_ERR("Pairing failed conn: %s, reason %d", addr, reason);

    // TODO: Disconnect
}

static struct bt_conn_auth_cb conn_auth_callbacks ={
    .cancel = auth_cancel,
    .pairing_confirm = pairing_confirm,
    .pairing_complete = pairing_complete,
    .pairing_failed = pairing_failed };

static void bt_receive_cb(struct bt_conn *conn, const uint8_t *const data,
    uint16_t len)
{

    // Forward it back if the evt handler is valid
    if (m_evt_cb)
        m_evt_cb(data, len);
}

static void bt_sent_cb(struct bt_conn *conn)
{
    // Check if empty
    if (k_msgq_num_used_get(&m_peripheral_event_queue) == 0)
    {
        return;
    }

    k_work_submit(&bt_send_work);
}

static struct bt_gatt_nus_cb nus_cb ={
    .received_cb = bt_receive_cb,
    .sent_cb = bt_sent_cb,
};

void ble_peripheral_ready()
{
    // Init nus
    bt_gatt_nus_init(&nus_cb);

    // Start advertising
    ble_peripheral_advertising_start();

    // Register callbacks
    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_cb_register(&conn_auth_callbacks);

    /* Initialize the Bluetooth mcumgr transport. */
    smp_bt_register();
}

void ble_peripheral_write(const uint8_t *data, uint16_t len)
{

    // If not valid connection return
    if (current_conn == NULL)
    {
        LOG_ERR("Current connection not valid!");
        return;
    }

    // Check if len > buffer size
    if (len > BLE_QUEUE_ITEM_SIZE)
    {
        LOG_ERR("Invalid data length %d > %d", len, BLE_QUEUE_ITEM_SIZE);
        return;
    }

    // Allocate memory for a tx_payload
    ble_fifo_data_t tx_payload;

    // Copy the contents
    memcpy(tx_payload.data, data, len);
    tx_payload.len = len;

    int err;

    // Copy the item to the msgq
    err = k_msgq_put(&m_peripheral_event_queue, &tx_payload, K_NO_WAIT);
    if (err)
    {
        LOG_ERR("Unable to queue item");
        return;
    }

    // Start work if it hasn't already
    k_work_submit(&bt_send_work);
}

void ble_peripheral_attach_handler(encoded_data_handler_t evt_cb)
{
    m_evt_cb = evt_cb;
}

void ble_peripheral_init()
{

    // Clear ready bit
    atomic_set(&m_ready, 0);
}

void ble_peripheral_disconnect()
{
    // TODO: disconect
}

bool ble_peripheral_is_connected(void)
{
    // Check if set up and not disconnected
    return (atomic_get(&m_ready) == 1);
}

#endif
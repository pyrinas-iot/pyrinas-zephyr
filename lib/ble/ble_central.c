/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if CONFIG_PYRINAS_CENTRAL_ENABLED

#include <zephyr.h>
#include <sys/byteorder.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include <ble/services/pyrinas.h>
#include <ble/ble_central.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(ble_central);

#define NUS_WRITE_TIMEOUT K_MSEC(150)

/* Struct def */
struct ble_c_connection
{
    /* Queue related*/
    struct k_msgq q;
    char __aligned(4) q_buf[BLE_CENTRAL_QUEUE_SIZE * sizeof(struct ble_fifo_data)];
    struct ble_fifo_data current_payload;

    /* Per connection */
    struct bt_conn *conn;
    uint16_t data_handle;
    uint16_t ccc_handle;

    /* Discover params*/
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_subscribe_params notif_params;
};

/* Used to track connection */
static atomic_t m_num_connected = ATOMIC_INIT(0);
static atomic_t m_force_disconnect = ATOMIC_INIT(0);
static struct ble_c_connection m_conns[CONFIG_BT_MAX_CONN];

/*UUIDs*/
static const struct bt_uuid *service_uuid = BT_UUID_PYRINAS_SERVICE;
static const struct bt_uuid *data_uuid = BT_UUID_PYRINAS_DATA;
static const struct bt_uuid *ccc_uuid = BT_UUID_GATT_CCC;

/* Static local handlers */
static encoded_data_handler_t m_evt_cb = NULL;

/* Work! */
static struct k_work_q *ble_work_q;

/* Related work handler for rx ring buf*/
static void bt_send_work_handler(struct k_work *work);
static void bt_scan_work_handler(struct k_work *work);
static struct k_work_delayable bt_send_work;
static struct k_work_delayable bt_scan_work;

/* Storing static config*/
static ble_central_config_t m_config;

static void bt_scan_work_handler(struct k_work *work)
{
    ble_central_scan_start();
}

static void bt_send_work_handler(struct k_work *work)
{
    int err;

    bool schedule_work = false;

    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
    {

        // Check for invalid connection
        if (m_conns[i].conn == NULL ||
            k_msgq_num_used_get(&m_conns[i].q) == 0)
        {
            continue;
        }

        struct bt_conn_info info = {0};
        bt_conn_get_info(m_conns[i].conn, &info);
        if (info.state != BT_CONN_STATE_CONNECTED)
        {
            LOG_WRN("Not connected!");
            bt_conn_disconnect(m_conns[i].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            break;
        }

        // Get the latest item
        err = k_msgq_get(&m_conns[i].q, &m_conns[i].current_payload, K_NO_WAIT);
        if (err)
        {
            LOG_WRN("%d Unable to get data from queue", i);
            continue;
        }

        LOG_DBG("Sending from %i", i);

        // Send the data
        // ! This call is asyncronous. Need to call semaphore and then release once data is sent.
        err = bt_gatt_write_without_response(m_conns[i].conn,
                                             m_conns[i].data_handle, m_conns[i].current_payload.data,
                                             m_conns[i].current_payload.len, false);
        if (err)
        {

            LOG_ERR("Unable to write without rsp. Code: %i", err);

            /* Schedule work */
            schedule_work = true;

            /*Break from for loop. Device is busy..*/
            break;
        }
        else
        {
            /* Still more data? Schedule work.*/
            if (k_msgq_num_used_get(&m_conns[i].q) > 0)
            {
                /* Schedule work */
                schedule_work = true;
            }
        }
    }

    if (schedule_work)
    {
        k_work_reschedule_for_queue(ble_work_q, &bt_send_work, K_MSEC(50));
    }
}

void ble_central_write(const uint8_t *data, uint16_t len)
{

    bool ready = false;

    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
    {

        // Check to make sure there's at least one active connection
        if (m_conns[i].conn != NULL)
        {
            ready = true;
            break;
        }
    }

    // If not valid connection return
    if (!ready)
    {
        return;
    }

    if (len > BLE_QUEUE_ITEM_SIZE)
    {
        LOG_ERR("Payload size too large!");
        return;
    }

    struct ble_fifo_data evt;

    // Copy over data to struct
    memcpy(evt.data, data, len);
    evt.len = len;

    // Then queue for each of the connections
    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
    {
        // Queue if ready
        if (m_conns[i].conn != NULL)
        {
            LOG_DBG("Queing to connection %d with %i", i, k_msgq_num_used_get(&m_conns[i].q));

            // Add struct to queue
            int err = k_msgq_put(&m_conns[i].q, &evt, K_NO_WAIT);
            if (err)
            {
                LOG_ERR("Unable to add outgoing event to queue!");

		/* Disconnect to fix? */
            }
        }
    }

    // Start the worker thread
    k_work_reschedule_for_queue(ble_work_q, &bt_send_work, K_NO_WAIT);
}

static uint8_t on_received(struct bt_conn *conn,
                           struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t len)
{

    // Sends the data forward if the callback is valid
    if (data && m_evt_cb)
    {
        m_evt_cb(data, len);
    }

    return BT_GATT_ITER_CONTINUE;
}

static uint8_t discover_func(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    int err;
    struct ble_c_connection *conn_data = NULL;

    if (!attr)
    {
        LOG_INF("Attributes not found!");
        (void)memset(params, 0, sizeof(*params));
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        return BT_GATT_ITER_STOP;
    }

    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
    {
        if (m_conns[i].conn == conn)
        {
            conn_data = &m_conns[i];
            break;
        }
    }

    if (conn_data == NULL)
    {
        LOG_ERR("No conn found!");
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);

    if (!bt_uuid_cmp(conn_data->discover_params.uuid, BT_UUID_PYRINAS_SERVICE))
    {
        conn_data->discover_params.uuid = data_uuid;
        conn_data->discover_params.start_handle = attr->handle + 1;
        conn_data->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

        err = bt_gatt_discover(conn, &conn_data->discover_params);
        if (err)
        {
            LOG_ERR("Discover failed (err %d)", err);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    }
    else if (!bt_uuid_cmp(conn_data->discover_params.uuid,
                          BT_UUID_PYRINAS_DATA))
    {
        /* Assign handle */
        conn_data->data_handle = bt_gatt_attr_value_handle(attr);

        conn_data->discover_params.uuid = ccc_uuid;
        conn_data->discover_params.start_handle = attr->handle + 2;
        conn_data->discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;

        err = bt_gatt_discover(conn, &conn_data->discover_params);
        if (err)
        {
            LOG_ERR("Discover failed (err %d)", err);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    }
    else
    {

        /* Assign handle */
        conn_data->ccc_handle = attr->handle;

        conn_data->notif_params.notify = on_received;
        conn_data->notif_params.value = BT_GATT_CCC_NOTIFY;
        conn_data->notif_params.value_handle = conn_data->data_handle;
        conn_data->notif_params.ccc_handle = conn_data->ccc_handle;
        atomic_set_bit(conn_data->notif_params.flags,
                       BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

        err = bt_gatt_subscribe(conn_data->conn, &conn_data->notif_params);
        if (err)
        {
            LOG_ERR("Subscribe failed (err %d)", err);
        }
        else
        {
            atomic_inc(&m_num_connected);

            /* Start scanning if we're < max connections */
            if (atomic_get(&m_num_connected) < m_config.device_count)
            {
                k_work_reschedule_for_queue(ble_work_q, &bt_scan_work, K_MSEC(50));
            }
        }

        (void)memset(params, 0, sizeof(*params));

        return BT_GATT_ITER_STOP;
    }

    return BT_GATT_ITER_STOP;
}

void ble_central_scan_stop(void)
{
    int err;

    LOG_INF("Scan stop!");

    /* Stop scanning */
    err = bt_le_scan_stop();
    if (err && (err != -EALREADY))
    {
        LOG_ERR("Stop LE scan failed (err %d)", err);
    }
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
                         struct net_buf_simple *ad)
{
    /* Default connection */
    struct bt_conn *conn = NULL;
    char addr_str[BT_ADDR_LE_STR_LEN];
    int err;

    bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
    LOG_DBG("Device found: %s (RSSI %d)", (char *)addr_str, rssi);

    /* Check whitelist  */
    bool found = false;
    for (int i = 0; i < m_config.device_count; i++)
    {
        if (bt_addr_le_cmp(&m_config.addr[i], addr) == 0)
        {
            found = true;
            break;
        }
    }

    if (!found)
        return;

    if (bt_le_scan_stop())
    {
        return;
    }

    LOG_INF("Connecting to: %s (RSSI %d)", addr_str, rssi);

    /* Then connect */
    struct bt_conn_le_create_param *create_params =
        BT_CONN_LE_CREATE_PARAM((BT_CONN_LE_OPT_CODED | BT_CONN_LE_OPT_NO_1M),
                                BT_GAP_SCAN_FAST_INTERVAL,
                                BT_GAP_SCAN_FAST_INTERVAL);

    err = bt_conn_le_create(addr, create_params, BT_LE_CONN_PARAM_DEFAULT, &conn);
    if (err)
    {
        LOG_ERR("Create conn to %s failed (%u)", addr_str, err);

        /* Start scanning if we're < max connections */
        if (atomic_get(&m_num_connected) < m_config.device_count)
        {
            k_work_reschedule_for_queue(ble_work_q, &bt_scan_work, K_MSEC(50));
        }
    }
}

void ble_central_scan_start()
{

    /* Force disconnect*/
    if (atomic_get(&m_force_disconnect) == 1)
    {
        LOG_INF("Force disconnect.");
        return;
    }

    /* Only scans if we have devices */
    if (m_config.device_count == 0)
    {
        LOG_INF("No devices to scan.");
        return;
    }

    /* Stop first to confirm .. */
    bt_le_scan_stop();

    int err = bt_le_scan_start(BT_LE_SCAN_CODED_ACTIVE, device_found);
    if (err && err != -EALREADY)
    {
        LOG_WRN("Scanning failed to start, err %d", err);
        return;
    }
    else if (err == 0)
    {
        LOG_INF("Scan start!");
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{

    LOG_INF("Disconnected. (reason 0x%02x)", reason);

    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
    {

        /* Make sure the conn is the correct one */
        if (m_conns[i].conn != conn)
        {
            continue;
        }

        /* unref and NULL */
        bt_conn_unref(m_conns[i].conn);
        m_conns[i].conn = NULL;

        /* Reset client */
        m_conns[i].data_handle = 0;
        m_conns[i].ccc_handle = 0;

        /* Purge data */
        k_msgq_purge(&m_conns[i].q);

        /* Reset ready flag */
        if (atomic_get(&m_num_connected))
        {
            atomic_dec(&m_num_connected);
        }

        // Break from loop
        break;
    }

    /* Start scanning if we're < max connections */
    if (atomic_get(&m_num_connected) < m_config.device_count)
    {
        k_work_reschedule_for_queue(ble_work_q, &bt_scan_work, K_MSEC(50));
    }
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
    int err;

    // Return if there's a connection error
    if (conn_err)
    {
        LOG_ERR("Failed to connect: %d", conn_err);

	// Disconnect on failure
        bt_conn_disconnect(conn, BT_HCI_ERR_INVALID_PARAM);

        return;
    }

    LOG_INF("Connected");

    // Iterate and find an open conn
    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
    {
        // Set to an unused conn
        if (m_conns[i].conn == NULL)
        {
            m_conns[i].conn = conn;

            /* Start discovery process */
            m_conns[i].discover_params.uuid = service_uuid;
            m_conns[i].discover_params.func = discover_func;
            m_conns[i].discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
            m_conns[i].discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
            m_conns[i].discover_params.type = BT_GATT_DISCOVER_PRIMARY;

            err = bt_gatt_discover(m_conns[i].conn, &m_conns[i].discover_params);
            if (err)
            {
                LOG_ERR("Discover failed (err %d)", err);
                return;
            }
            break;
        }
    }
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
                             enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err)
    {
        LOG_INF("Security changed: %s level %u", addr, level);
    }
    else
    {
        LOG_ERR("Security failed: %s level %u err %d", addr, level,
                err);

        // Disconnect on security failure
        int err = bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        if (err && (err != -ENOTCONN))
        {
            LOG_ERR("Cannot disconnect peer (err:%d)", err);
        }
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed};

void ble_central_ready(void)
{
    LOG_INF("Bluetooth ready");
}

void ble_central_attach_handler(encoded_data_handler_t evt_cb)
{
    m_evt_cb = evt_cb;
}

int ble_central_init(struct k_work_q *p_ble_work_q, ble_central_config_t *p_init)
{

    // Throw an error if NULL
    __ASSERT(p_init != NULL, "Error: Invalid param.");

    // Check if the work queue is null
    __ASSERT(p_ble_work_q != NULL, "Error: Invalid param.");

    // Assign it!
    ble_work_q = p_ble_work_q;

    // Set this count to 0
    atomic_set(&m_num_connected, 0);

    /* Set up work */
    k_work_init_delayable(&bt_send_work, bt_send_work_handler);
    k_work_init_delayable(&bt_scan_work, bt_scan_work_handler);

    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
    {

        /* Set conn to NULL*/
        m_conns[i].conn = NULL;

        // Init the msgq
        k_msgq_init(&m_conns[i].q, m_conns[i].q_buf, sizeof(struct ble_fifo_data), BLE_CENTRAL_QUEUE_SIZE);
    }

    /* Callbacks for conection status */
    bt_conn_cb_register(&conn_callbacks);

    // Copy the config over
    m_config = *p_init;

    return 0;
}

int ble_central_is_connected()
{
    return (int)atomic_get(&m_num_connected);
}

void ble_central_set_whitelist(ble_central_config_t *config)
{
    int err;

    /* Compare to see if there were changes */
    if (memcmp(&m_config, config, sizeof(ble_central_config_t)) == 0)
    {
        LOG_INF("Same config. No changes necessary.");
        return;
    }

    atomic_set(&m_force_disconnect, 1);

    /* Stop scanning */
    err = bt_le_scan_stop();
    if (err && (err != -EALREADY))
    {
        LOG_ERR("Stop LE scan failed (err %d)", err);
    }

    /* Disconnect from all */
    for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
    {
        if (m_conns[i].conn != NULL)
        {
            bt_conn_disconnect(m_conns[i].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    }

    /*Set devices by copying*/
    m_config = *config;

    LOG_INF("Device count: %d", m_config.device_count);

    k_work_reschedule_for_queue(ble_work_q, &bt_scan_work, K_MSEC(50));

    atomic_set(&m_force_disconnect, 0);
}

#endif

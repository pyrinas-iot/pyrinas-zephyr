/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>

#include <proto/command.pb.h>
#include <pb_decode.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/gatt.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_vs.h>
#include <bluetooth/services/nus.h>
#include <bluetooth/services/nus_c.h>

#include <settings/settings.h>

#include <sys/byteorder.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(ble_central);

#include <ble/ble_central.h>
#include <ble/ble_char_info.h>

#define NUS_WRITE_TIMEOUT K_MSEC(150)

/* Useed to copy and queue messages */
K_MSGQ_DEFINE(m_central_event_queue, sizeof(ble_fifo_data_t), 20, BLE_QUEUE_ALIGN);
K_SEM_DEFINE(nus_c_write_sem, 1, 1);

/* Used to track connection */
static atomic_t m_num_connected;
static struct bt_conn *current_conn;
static struct bt_conn *conns[CONFIG_BT_MAX_CONN];
static struct bt_gatt_nus_c gatt_nus_c;

/* Determines if central is ready for action */
static atomic_t m_ready;

/* Static local handlers */
static encoded_data_handler_t m_evt_cb = NULL;

/* Related work handler for rx ring buf*/
static void bt_send_work_handler(struct k_work *work);
static K_WORK_DEFINE(bt_send_work, bt_send_work_handler);

/* Storing static config*/
static ble_central_init_t m_config;

static void bt_send_work_handler(struct k_work *work)
{
	int err;

	// Check for invalid connection
	if (current_conn == NULL)
	{
		LOG_WRN("Connected not valid");
		return;
	}

	// Tries to take immediately, if not, there's an operation still going.
	// This will get triggered again once it's done..
	err = k_sem_take(&nus_c_write_sem, K_NO_WAIT);
	if (err)
	{
		LOG_WRN("Unable to take semaphore.\n");
		return;
	}

	// Static event
	static ble_fifo_data_t ble_payload;

	// Get the latest item
	err = k_msgq_get(&m_central_event_queue, &ble_payload, K_NO_WAIT);
	if (err)
	{
		LOG_WRN("Unable to get data from queue");
		k_sem_give(&nus_c_write_sem);
	}

	// Send the data
	// ! This call is asyncronous. Need to call semaphore and then release once data is sent.
	err = bt_gatt_nus_c_send(&gatt_nus_c, ble_payload.data, ble_payload.len);
	if (err)
	{
		LOG_ERR("Failed to send data over BLE connection"
						"(err %d)",
						err);
		k_sem_give(&nus_c_write_sem);
	}
}

static void scan_connecting(struct bt_scan_device_info *device_info,
														struct bt_conn *conn)
{
	current_conn = bt_conn_ref(conn);
}

BT_SCAN_CB_INIT(scan_cb, NULL, NULL,
								NULL, scan_connecting);

static void ble_central_scan_init(void)
{
	int err;

	// Active scanning with phy coded enabled
	// BT_LE_SCAN_OPT_CODED | BT_LE_SCAN_OPT_NO_1M,
	struct bt_le_scan_param scan_param = {
			.type = BT_LE_SCAN_TYPE_ACTIVE,
			.options = BT_LE_SCAN_OPT_NONE,
			.interval = BT_GAP_SCAN_FAST_INTERVAL,
			.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	// !Note: this sets the default connection interval. If it needs
	// !to be sped up, this is the place
	struct bt_scan_init_param scan_init = {
			.connect_if_match = 1,
			.scan_param = &scan_param,
			.conn_param = BT_LE_CONN_PARAM_DEFAULT,
	};

	// Init scanning
	bt_scan_init(&scan_init);
	bt_scan_cb_register(&scan_cb);

	// Add a filter
	// TODO: loop through all potential device IDs  and add those filters
	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_NAME, "Pyrinas");
	if (err)
	{
		LOG_WRN("Scanning filters cannot be set. Err: %d", err);
		return;
	}

	// Enable said filter
	err = bt_scan_filter_enable(BT_SCAN_NAME_FILTER, false);
	if (err)
	{
		LOG_WRN("Filters cannot be turned on. Err: %d\n", err);
		return;
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

	printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Pairing failed conn: %s, reason %d\n", addr, reason);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
		.cancel = auth_cancel,
		.pairing_confirm = pairing_confirm,
		.pairing_complete = pairing_complete,
		.pairing_failed = pairing_failed};

void ble_central_write(const u8_t *data, u16_t len)
{

	// If not valid connection return
	if (current_conn == NULL)
	{
		LOG_WRN("Invalid connection.");
		return;
	}

	if (len > BLE_QUEUE_ITEM_SIZE)
	{
		LOG_ERR("Payload size too large!");
		return;
	}

	ble_fifo_data_t evt;

	// Copy over data to struct
	memcpy(evt.data, data, len);
	evt.len = len;

	// Add struct to queue
	int err = k_msgq_put(&m_central_event_queue, &evt, K_NO_WAIT);
	if (err)
	{
		LOG_ERR("Unable to add item to queue!");
	}

	// Start the worker thread
	k_work_submit(&bt_send_work);
}

static void discovery_completed(struct bt_gatt_dm *dm, void *context)
{
	struct bt_gatt_nus_c *nus_c = context;

	LOG_INF("Discovery complete!");

	bt_gatt_nus_c_handles_assign(dm, nus_c);
	bt_gatt_nus_c_tx_notif_enable(nus_c);

	bt_gatt_dm_data_release(dm);

	// Establish pairing/security
	int err = bt_conn_set_security(current_conn, BT_SECURITY_L2);
	if (err)
	{
		printk("Failed to set security: %d\n", err);
	}

	// TODO: check in m_clients

	// TODO: handle any security stuff/whitelist

	// TODO: disconnect if if one of the issues has an error above

	// TODO: Then check if we can start scanning again
	// compare with m_config.device_count
}

static void discovery_service_not_found(struct bt_conn *conn, void *ctx)
{
	LOG_WRN("Pyrinas data service not found!");

	// Disconnect from device
	int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (err && (err != -ENOTCONN))
	{
		LOG_ERR("Cannot disconnect peer (err:%d)", err);
	}
}

static void discovery_error_found(struct bt_conn *conn, int err, void *ctx)
{
	LOG_WRN("The discovery procedure failed, err %d\n", err);

	// Disconnect from device
	int ret = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	if (ret && (ret != -ENOTCONN))
	{
		LOG_ERR("Cannot disconnect peer (err:%d)", ret);
	}
}

static struct bt_gatt_dm_cb discovery_cb = {
		.completed = discovery_completed,
		.service_not_found = discovery_service_not_found,
		.error_found = discovery_error_found,
};

static void gatt_discover(struct bt_conn *conn)
{
	int err;

	if (conn != current_conn)
	{
		return;
	}

	err = bt_gatt_dm_start(conn,
												 BT_UUID_NUS_SERVICE,
												 &discovery_cb,
												 &gatt_nus_c);
	if (err)
	{
		printk("could not start the discovery procedure, error "
					 "code: %d\n",
					 err);
	}
}

void ble_central_scan_start()
{
	int err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err)
	{
		LOG_WRN("Scanning failed to start, err %d\n", err);
		return;
	}

	LOG_INF("Scanning...");
}

static void connected(struct bt_conn *conn, u8_t conn_err)
{
	int err;

	// Return if there's a connection error
	if (conn_err)
	{
		LOG_ERR("Failed to connect: %d", conn_err);

		// Re-start scanning
		ble_central_scan_start();
		return;
	}

	LOG_INF("Connected");
	gatt_discover(conn);

	// Stp scanning
	err = bt_scan_stop();
	if ((!err) && (err != -EALREADY))
	{
		printk("Stop LE scan failed (err %d)\n", err);
	}
}

static void disconnected(struct bt_conn *conn, u8_t reason)
{

	printk("Disconnected. (reason 0x%02x)\n", reason);

	if (current_conn != conn)
	{
		return;
	}

	bt_conn_unref(current_conn);
	current_conn = NULL;

	atomic_set(&m_ready, 0);

	// Start scanning again and re-connect if found
	ble_central_scan_start();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
														 enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err)
	{
		printk("Security changed: %s level %u\n", addr, level);

		atomic_set(&m_ready, 1);
	}
	else
	{
		printk("Security failed: %s level %u err %d\n", addr, level,
					 err);

		// Disconnect on security failure
		int err = bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
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

static void ble_data_sent(void *ctx, u8_t err, const u8_t *const data, u16_t len)
{

	// Release semaphore
	k_sem_give(&nus_c_write_sem);

	// If there's nothing left return
	if (k_msgq_num_used_get(&m_central_event_queue) == 0)
	{
		return;
	}

	k_work_submit(&bt_send_work);
}

static u8_t ble_data_received(void *ctx, const u8_t *const data, u16_t len)
{

	// Sends the data forward if the callback is valid
	if (m_evt_cb)
	{
		m_evt_cb(data, len);
	}

	return BT_GATT_ITER_CONTINUE;
}

struct bt_gatt_nus_c_init_param nus_c_init_obj = {
		.cbs = {
				.data_received = ble_data_received,
				.data_sent = ble_data_sent,
		}};

void ble_central_ready(void)
{
	LOG_INF("Bluetooth ready");

	int err = bt_gatt_nus_c_init(&gatt_nus_c, &nus_c_init_obj);
	if (err)
	{
		printk("NUS Client initialization failed (err %d)\n", err);
		return;
	}

	ble_central_scan_start();
}

void ble_central_attach_handler(encoded_data_handler_t evt_cb)
{
	m_evt_cb = evt_cb;
}

int ble_central_init(ble_central_init_t *p_init)
{

	LOG_INF("ble_central_init");

	// Throw an error if NULL
	if (p_init == NULL)
	{
		__ASSERT(p_init == NULL, "Error: Invalid param.\n");
	}

	int err = bt_conn_auth_cb_register(&conn_auth_callbacks);
	if (err)
	{
		printk("Failed to register authorization callbacks.\n");
		return err;
	}

	if (IS_ENABLED(CONFIG_SETTINGS))
	{
		settings_load();
	}

	// Clear ready bit
	atomic_set(&m_ready, 0);
	atomic_set(&m_num_connected, 0);

	/* Callbacks for conection status */
	bt_conn_cb_register(&conn_callbacks);

	/* Initialize scanning filters */
	ble_central_scan_init();

	// Copy the config over
	m_config = *p_init;

	return 0;
}

bool ble_central_is_connected()
{
	// Returns true if connected
	return (atomic_get(&m_ready) == 1);
}
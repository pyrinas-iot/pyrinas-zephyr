/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#if CONFIG_PYRINAS_CENTRAL_ENABLED

#include <zephyr.h>
#include <sys/byteorder.h>
#include <settings/settings.h>

#include <proto/command.pb.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/scan.h>
#include <bluetooth/services/nus.h>
#include <bluetooth/services/nus_c.h>

#include <ble/ble_central.h>
#include <ble/ble_char_info.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(ble_central);

#define NUS_WRITE_TIMEOUT K_MSEC(150)

/* Struct def */
struct ble_nus_c_connection
{
	/* Tracking valid/active connection*/
	atomic_t ready;

	/* Conn tracking */
	struct bt_conn *conn;

	/* Semaphore */
	struct k_sem sem;

	/* Queue related*/
	struct k_msgq q;
	char __aligned(4) q_buf[BLE_CENTRAL_QUEUE_SIZE * sizeof(ble_fifo_data_t)];

	/* NUS Client */
	struct bt_gatt_nus_c nus_c;
};

/* Used to track connection */
static atomic_t m_num_connected;
static struct ble_nus_c_connection m_conns[CONFIG_BT_MAX_CONN];

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

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{

		// Check for invalid connection
		if (!(atomic_get(&m_conns[i].ready) == 1) ||
				m_conns[i].conn == NULL ||
				k_msgq_num_used_get(&m_conns[i].q) == 0)
		{
			continue;
		}

		LOG_DBG("%d: ready!", i);

		// Tries to take immediately, if not, there's an operation still going.
		// This will get triggered again once it's done..
		err = k_sem_take(&m_conns[i].sem, K_NO_WAIT);
		if (err)
		{
			LOG_WRN("Unable to take semaphore.\n");
			continue;
		}

		LOG_DBG("%d: sem take!", i);

		// Static event
		static ble_fifo_data_t ble_payload;

		// Get the latest item
		err = k_msgq_get(&m_conns[i].q, &ble_payload, K_NO_WAIT);
		if (err)
		{
			LOG_WRN("%d Unable to get data from queue", i);
			k_sem_give(&m_conns[i].sem);
			continue;
		}

		LOG_DBG("%d: msg get!", i);

		// Send the data
		// ! This call is asyncronous. Need to call semaphore and then release once data is sent.
		err = bt_gatt_nus_c_send(&m_conns[i].nus_c, ble_payload.data, ble_payload.len);
		if (err)
		{
			LOG_ERR("Failed to send data over BLE connection"
							"(err %d)",
							err);
			k_sem_give(&m_conns[i].sem);
		}

		LOG_DBG("%d: msg send!", i);
	}
}

static void scan_filter_match(struct bt_scan_device_info *device_info,
															struct bt_scan_filter_match *filter_match,
															bool connectable)
{
	LOG_INF("Found and is %sconnectable", connectable ? "" : "not ");
}

static void scan_connecting(struct bt_scan_device_info *device_info,
														struct bt_conn *conn)
{

	// Iterate and find an open conn
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{
		// Set to an unused conn
		if (m_conns[i].conn == NULL)
		{
			m_conns[i].conn = bt_conn_ref(conn);
			break;
		}
	}
}

BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL,
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

	bool ready = false;

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{

		// Check to make sure there's at least one active connection
		if (atomic_get(&m_conns[i].ready) == 1)
		{
			ready = true;
			break;
		}
	}

	// If not valid connection return
	if (!ready)
	{
		LOG_WRN("Invalid connection(s).");
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

	// Then queue for each of the connections
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{
		// Queue if ready
		if (m_conns[i].ready)
		{
			// Add struct to queue
			int err = k_msgq_put(&m_conns[i].q, &evt, K_NO_WAIT);
			if (err)
			{
				LOG_ERR("Unable to add item to queue!");
			}
		}
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

	struct ble_nus_c_connection *dev_conn;
	bool found = false;

	// Check if the conns are equal then set nus_c
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{
		// Copy the context address over
		if (m_conns[i].conn == nus_c->conn)
		{
			// This is used to start sec
			dev_conn = &m_conns[i];

			// Copy the context
			m_conns[i].nus_c = *nus_c;

			// We've found it!
			found = true;

			break;
		}
	}

	if (!found)
	{
		LOG_ERR("Not found but should be...");
		return;
	}

	// Establish pairing/security
	int err = bt_conn_set_security(dev_conn->conn, BT_SECURITY_L2);
	if (err)
	{
		printk("Failed to set security: %d\n", err);
	}
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

	bool found = false;

	struct ble_nus_c_connection *dev_conn;

	// Check if the conns are equal then set nus_c
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{
		// Copy the context address over
		if (m_conns[i].conn == conn)
		{
			// This is used to start sec
			dev_conn = &m_conns[i];

			found = true;
		}
	}

	if (!found)
	{
		LOG_WRN("Should be found!");
		return;
	}

	err = bt_gatt_dm_start(dev_conn->conn,
												 BT_UUID_NUS_SERVICE,
												 &discovery_cb,
												 &dev_conn->nus_c);
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
	// int err;

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

	// TODO: only if there are more to get
	// // Stp scanning
	// err = bt_scan_stop();
	// if ((!err) && (err != -EALREADY))
	// {
	// 	printk("Stop LE scan failed (err %d)\n", err);
	// }
}

static void disconnected(struct bt_conn *conn, u8_t reason)
{

	printk("Disconnected. (reason 0x%02x)\n", reason);

	bool advertise = false;

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{

		// Make sure the conn is the correct one
		if (m_conns[i].conn != conn)
		{
			continue;
		}

		// unref and NULL
		bt_conn_unref(m_conns[i].conn);
		m_conns[i].conn = NULL;

		// Purge data
		k_msgq_purge(&m_conns[i].q);

		// Reset ready flag
		atomic_set(&m_conns[i].ready, 0);
		atomic_dec(&m_num_connected);

		// Set this flag to start adv at the end
		advertise = true;

		// Break from loop
		break;
	}

	// Start scanning again and re-connect if found
	if (advertise)
		ble_central_scan_start();
}

static void security_changed(struct bt_conn *conn, bt_security_t level,
														 enum bt_security_err err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (!err)
	{
		LOG_INF("Security changed: %s level %u\n", log_strdup(addr), level);

		for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
		{
			if (m_conns[i].conn == conn)
			{
				atomic_set(&m_conns[i].ready, 1);
				atomic_inc(&m_num_connected);

				// Start scanning if we're < max connections
				if (atomic_get(&m_num_connected) < CONFIG_BT_MAX_CONN)
				{
					ble_central_scan_start();
				}

				break;
			}
		}
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

	bool has_work = false;

	// See which connection this belongs to
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{

		// Find the nus_c struct by the context
		if (&m_conns[i].nus_c == ctx)
		{

			if (k_msgq_num_used_get(&m_conns[i].q))
			{
				has_work = true;
			}

			// Release so more messages can be sent
			k_sem_give(&m_conns[i].sem);

			break;
		}
	}

	// Check if there's more work to do
	if (has_work)
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

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{

		int err = bt_gatt_nus_c_init(&m_conns[i].nus_c, &nus_c_init_obj);
		if (err)
		{
			printk("NUS Client initialization failed (err %d)\n", err);
			return;
		}
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

	// Set this count to 0
	atomic_set(&m_num_connected, 0);

	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{
		// Set the active atomic var to 0
		atomic_set(&m_conns[i].ready, 0);

		// Init the semaphore
		err = k_sem_init(&m_conns[i].sem, 1, 1);
		if (err)
		{
			LOG_ERR("Unable to init semaphore.");
			return err;
		}

		// Init the msgq
		k_msgq_init(&m_conns[i].q, m_conns[i].q_buf, sizeof(ble_fifo_data_t), BLE_CENTRAL_QUEUE_SIZE);
	}

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

	bool ready = false;
	for (int i = 0; i < CONFIG_BT_MAX_CONN; i++)
	{
		if (atomic_get(&m_conns[i].ready) == 1)
		{
			ready = true;
			break;
		}
	}

	// Returns true if connected
	return ready;
}

#endif
/*
 * Copyright (c) 2020 Circuit Dojo LLC
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <zephyr.h>

#include <device.h>
#include <devicetree.h>

#include <drivers/gpio.h>

#include <bluetooth/gatt.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>

#include <sys/byteorder.h>

#include "ble.h"

/* Thinghy advertisement UUID */
#define BT_UUID_THINGY                                                \
	BT_UUID_DECLARE_128(0x42, 0x00, 0x74, 0xA9, 0xFF, 0x52, 0x10, 0x9B, \
											0x33, 0x49, 0x35, 0x9B, 0x00, 0x01, 0x68, 0xEF)

/* Pyrinas service UUID */
#define BT_UUID_TMS                                                   \
	BT_UUID_DECLARE_128(0x42, 0x00, 0x74, 0xA9, 0xFF, 0x52, 0x10, 0x9B, \
											0x33, 0x49, 0x35, 0x9B, 0x00, 0x04, 0x68, 0xEF)

/* Thingy characteristic UUID */
#define BT_UUID_TOC                                                   \
	BT_UUID_DECLARE_128(0x42, 0x00, 0x74, 0xA9, 0xFF, 0x52, 0x10, 0x9B, \
											0x33, 0x49, 0x35, 0x9B, 0x03, 0x04, 0x68, 0xEF)

// Callbacks that get associated with `scan_cb`
// BT_SCAN_CB_INIT(scan_cb, scan_filter_match, NULL, NULL, NULL);

/* Storing static config*/
static ble_init_t m_config;

void scan_start(void)
{
	int err;

	// Active scanning with phy coded enabled
	struct bt_le_scan_param scan_param = {
			.type = BT_LE_SCAN_TYPE_ACTIVE,
			.options = BT_LE_SCAN_OPT_CODED,
			.interval = BT_GAP_SCAN_FAST_INTERVAL,
			.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	struct bt_scan_init_param scan_init = {
			.connect_if_match = 1,
			.scan_param = &scan_param,
			.conn_param = BT_LE_CONN_PARAM_DEFAULT,
	};

	bt_scan_init(&scan_init);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_THINGY);
	if (err)
	{
		printk("Scanning filters cannot be set\n");
		return;
	}

	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
	if (err)
	{
		printk("Filters cannot be turned on\n");
	}

	err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);
	if (err)
	{
		printk("Scanning failed to start, err %d\n", err);
	}

	printk("Scanning...\n");
}

static void discovery_completed(struct bt_gatt_dm *disc, void *ctx)
{
	int err;

	printk("Discovery complete!\n");

release:
	err = bt_gatt_dm_data_release(disc);
	if (err)
	{
		printk("Could not release discovery data, err: %d\n", err);
	}
}

static void discovery_service_not_found(struct bt_conn *conn, void *ctx)
{
	printk("Thingy orientation service not found!\n");
}

static void discovery_error_found(struct bt_conn *conn, int err, void *ctx)
{
	printk("The discovery procedure failed, err %d\n", err);
}

static struct bt_gatt_dm_cb discovery_cb = {
		.completed = discovery_completed,
		.service_not_found = discovery_service_not_found,
		.error_found = discovery_error_found,
};

static void connected(struct bt_conn *conn, u8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	// Return if there's a connection error
	if (conn_err)
	{
		printk("Failed to connect: %d\n", conn_err);
		return;
	}

	printk("Connected: %s\n", addr);

	err = bt_gatt_dm_start(conn, BT_UUID_TMS, &discovery_cb, NULL);
	if (err)
	{
		printk("Could not start service discovery, err %d\n", err);
	}
}

static void disconnected(struct bt_conn *conn, u8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected: %s (reason 0x%02x)\n", addr, reason);

	// Start scanning again and re-connect if foudn
	scan_start();
}

static struct bt_conn_cb conn_callbacks = {
		.connected = connected,
		.disconnected = disconnected,
};

static void ble_ready(int err)
{
	printk("Bluetooth ready\n");

	bt_conn_cb_register(&conn_callbacks);
	scan_start();
}

void ble_init(ble_init_t *p_init)
{
	int err;

	// Throw an error if NULL
	if (p_init == NULL)
	{
		__ASSERT(port, "Error: Invalid param.\n");
	}

	// Get the port involved
	struct device *port;
	port = device_get_binding(CONFIG_HCI_NCP_RST_PORT);
	__ASSERT(port, "Error: Bad port for boot HCI reset.\n");

	err = gpio_pin_configure(port, CONFIG_HCI_NCP_RST_PIN, GPIO_OUTPUT_INACTIVE);
	__ASSERT(err == 0, "Error: Unable to configure pin: %d", err);

	printk("Initializing Bluetooth..\n");
	err = bt_enable(ble_ready);
	__ASSERT(err == 0, "Error: Bluetooth init failed (err %d)\n", err);

	// Delay so both IC's are in sync
	k_msleep(1000);

	// Release
	err = gpio_pin_configure(port, CONFIG_HCI_NCP_RST_PIN, GPIO_DISCONNECTED);
	__ASSERT(err == 0, "Error: Unable to configure pin: %d", err);
}

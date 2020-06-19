/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_M_CENTRAL_H
#define BLE_M_CENTRAL_H

#include <bluetooth/gatt.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt_dm.h>
#include <bluetooth/scan.h>

#include "ble_settings.h"
#include "ble_handlers.h"

/* Struct for initailizing bluetooth central */
typedef struct
{
    char addr[BLE_SETTINGS_MAX_CONNECTIONS][BT_ADDR_LE_STR_LEN];
    uint8_t device_count;
} ble_central_init_t;

typedef struct
{
    u8_t *data;
    u16_t len;
} ble_central_broadcast_t;

//TODO: document this.
bool ble_central_is_connected(void);
void ble_central_disconnect(void);
void ble_central_attach_handler(encoded_data_handler_t raw_evt_handler);
void ble_central_write(const u8_t *data, u16_t size);
void ble_central_scan_start(void);
int ble_central_init(ble_central_init_t *init);
void ble_central_ready(void);

#endif
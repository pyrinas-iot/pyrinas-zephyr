/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_M_CENTRAL_H
#define BLE_M_CENTRAL_H

#include <ble/ble_settings.h>
#include <ble/ble_handlers.h>

#define BLE_CENTRAL_QUEUE_SIZE 10
#define BLE_CENTRAL_ADDR_STR_LEN 30

/* Struct for initailizing bluetooth central */
typedef struct
{
    char addr[BLE_SETTINGS_MAX_CONNECTIONS][BLE_CENTRAL_ADDR_STR_LEN];
    uint8_t device_count;
} ble_central_init_t;

typedef struct
{
    uint8_t *data;
    uint16_t len;
} ble_central_broadcast_t;

//TODO: document this.
bool ble_central_is_connected(void);
void ble_central_disconnect(void);
void ble_central_attach_handler(encoded_data_handler_t raw_evt_handler);
void ble_central_write(const uint8_t *data, uint16_t size);
void ble_central_scan_start(void);
int ble_central_init(ble_central_init_t *init);
void ble_central_ready(void);
void ble_central_process(void);

#endif
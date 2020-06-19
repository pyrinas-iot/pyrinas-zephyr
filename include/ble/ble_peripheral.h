/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_M_PERIPHERAL_H
#define BLE_M_PERIPHERAL_H

#include "ble_handlers.h"

//TODO document
bool ble_peripheral_is_connected(void);
void ble_peripheral_disconnect(void);
void ble_peripheral_attach_handler(encoded_data_handler_t raw_evt_handler);
void ble_peripheral_write(const u8_t *data, u16_t size);
void ble_peripheral_advertising_start(void);
void ble_peripheral_init(void);
void ble_peripheral_ready(void);

#endif
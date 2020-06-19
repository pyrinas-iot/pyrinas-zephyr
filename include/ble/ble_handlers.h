/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_HANDLERS_H
#define BLE_HANDLERS_H

#include <proto/command.pb.h>

/**@brief Callback to BLE module. */
typedef void (*ble_ready_t)(void);

// TODO: duplicate handlers that are doing the same thing as Central and Peripheral
/**@brief Subscription handler definition. */
typedef void (*susbcribe_handler_t)(char *name, char *data);

/**@brief Raw subscription handler definition. */
typedef void (*raw_susbcribe_handler_t)(protobuf_event_t *evt);

/**@brief Raw subscription handler definition. */
typedef void (*encoded_data_handler_t)(const char *data, uint16_t len);

#endif
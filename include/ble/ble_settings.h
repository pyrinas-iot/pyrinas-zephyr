/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_SETTINGS_H
#define BLE_SETTINGS_H

// Alignment of data in queue
#define BLE_QUEUE_ALIGN 4

// Calculate Queue Size
#if protobuf_event_t_size % BLE_QUEUE_ALIGN == 0
#define BLE_QUEUE_ITEM_SIZE protobuf_event_t_size
#else
#define BLE_QUEUE_ITEM_SIZE ((BLE_QUEUE_ALIGN - (protobuf_event_t_size % BLE_QUEUE_ALIGN)) + protobuf_event_t_size)
#endif

#if BLE_QUEUE_ITEM_SIZE % BLE_QUEUE_ALIGN != 0
#error something went wrong here!
#endif

// TODO: can't be set like this. This is platform and compiler dependant.
#define BLE_INCOMING_PROTOBUF_SIZE 168

// Variable data element
typedef struct
{
  void *fifo_reserved;
  char data[BLE_QUEUE_ITEM_SIZE];
  uint16_t len;
} ble_fifo_data_t;

#define BLE_SETTINGS_MAX_CONNECTIONS 12
#define BLE_SETTINGS_MAX_SUBSCRIPTIONS 12

#endif
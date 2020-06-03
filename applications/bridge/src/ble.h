/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _BLE_H_
#define _BLE_H_

#include "ble_handlers.h"
#include "proto/command.pb.h"

/**@brief Different device "modes"
 */
typedef enum
{
  ble_mode_peripheral = 0,
  ble_mode_central = 1,
} ble_mode_t;

/**@brief BLE callback function to main context.
 */

/**@brief Struct for initialization of BLE stack.
 */
typedef struct
{
  ble_mode_t mode;
  bool long_range;
  // union {
  //   ble_central_init_t config;
  // };

} ble_init_t;

#define BLE_STACK_PERIPH_DEF(X) ble_init_t X = {.mode = ble_mode_peripheral, .long_range = true}
#define BLE_STACK_CENTRAL_DEF(X) ble_init_t X = {.mode = ble_mode_central, .long_range = true}

/**@brief Function for terminating connection with a BLE peripheral device.
 */
void ble_disconnect(void);

/**@brief Function for initializing the BLE stack.
 */
void ble_init(ble_init_t *init);

/**@brief Function for starting the scanning.
 */
void scan_start(void);

/**@brief Function for publishing.
 */
void ble_publish(char *name, char *data);

// TODO: document this
void ble_publish_raw(protobuf_event_t event);

// TODO: document this
void ble_subscribe(char *name, susbcribe_handler_t handler);

// TODO: document this
void ble_subscribe_raw(raw_susbcribe_handler_t handler);

// TODO: document this
void advertising_start(void);

/**@brief Function for checking the connection state.
 *
 * @retval true     If peripheral device is connected.
 * @retval false    If peripheral device is not connected.
 */
bool ble_is_connected(void);

// TODO: document this
void ble_process();

#endif /* _BLE_H_ */

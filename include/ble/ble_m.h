/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**@brief     Application BLE module.
 *
 * @details   This module contains most of the functions used
 *            by the application to manage BLE stack events
 *            and BLE connections.
 */

#ifndef BLE_M_H__
#define BLE_M_H__

#include <ble/ble_central.h>
#include <ble/ble_settings.h>
#include <ble/ble_handlers.h>

/**@brief Struct for tracking callbacks
 */
typedef struct
{
  susbcribe_handler_t evt_handler;
  pyrinas_event_name_data_t name;
} ble_subscription_handler_t;

typedef struct
{
  ble_subscription_handler_t subscribers[BLE_SETTINGS_MAX_SUBSCRIPTIONS];
  uint8_t count;
} ble_subscription_list_t;

/**@brief Different device "modes"
 */
typedef enum
{
  ble_mode_peripheral,
  ble_mode_central,
} ble_mode_t;

/**@brief BLE callback function to main context.
 */

/**@brief Struct for initialization of BLE stack.
 */
typedef struct
{
  ble_mode_t mode;
  union
  {
    ble_central_config_t central_config;
  };

} ble_stack_init_t;

/**@brief Function for terminating connection with a BLE peripheral device.
 */
void ble_disconnect(void);

/**@brief Function for initializing the BLE stack.
 *
 * @details Initializes the SoftDevice and the BLE event interrupts.
 */
void ble_stack_init(ble_stack_init_t *init);

/**@brief Function for starting the scanning.
 */
void scan_start(void);

/**@brief Function for publishing.
 */
void ble_publish(char *name, char *data);

// TODO: document this
void ble_publish_raw(pyrinas_event_t *event);

// TODO: document this
void ble_subscribe(char *name, susbcribe_handler_t handler);

// TODO: document this
void ble_subscribe_raw(raw_susbcribe_handler_t handler);

// TODO: document this
void advertising_start(void);

void ble_erase_bonds(void);

/**@brief Function for checking the connection state.
 *
 * @retval true     If peripheral device is connected.
 * @retval false    If peripheral device is not connected.
 */
bool ble_is_connected(void);

/**@brief Function for obtaining connection handle.
 *
 * @return Returns connection handle.
 */
uint16_t ble_get_conn_handle(void);

#endif // BLE_M_H__

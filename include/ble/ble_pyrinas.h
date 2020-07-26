/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef PYRINAS_INCLUDE_BLUETOOTH_SERVICE_PYRINAS_H_
#define PYRINAS_INCLUDE_BLUETOOTH_SERVICE_PYRINAS_H_

#include <ble/ble_handlers.h>

/**
 * @brief Pyrinas General Purpose Data Service
 * @defgroup bt_gatt_pyrinas General Purpose Data Service
 * @ingroup bluetooth
 * @{
 *
 * [Experimental] Under active development.
 */

#ifdef __cplusplus
extern "C"
{
#endif

  /** @brief Notify on new data.
 *
 * This will send a GATT notification to all current subscribers.
 *
 *  @param data Raw data sent
 *  @param length Size of data sent
 *
 *  @return Zero in case of success and error code in case of error.
 */
  int bt_gatt_pyrinas_notify(char *data, uint16_t length);

  /** @brief Sets the callback
 *
 * This will set a callback that passes a protobuf_event_t.
 *
 *  @param cb Callback in question
 *
 */
  void bt_gatt_pyrinas_set_callback(raw_susbcribe_handler_t cb);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* PYRINAS_INCLUDE_BLUETOOTH_SERVICE_PYRINAS_H_ */

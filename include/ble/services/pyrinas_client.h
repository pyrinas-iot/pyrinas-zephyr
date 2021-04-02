/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef BT_PYRINAS_CLIENT_H_
#define BT_PYRINAS_CLIENT_H_

/**
 * @file
 * @defgroup bt_pyrinas_client Bluetooth LE GATT PYRINAS Client API
 * @{
 * @brief API for the Bluetooth LE GATT Nordic UART Service (PYRINAS) Client.
 */

#ifdef __cplusplus
extern "C"
{
#endif

#include <bluetooth/gatt.h>
#include <bluetooth/conn.h>
#include <bluetooth/gatt_dm.h>

    /** @brief Handles on the connected peer device that are needed to interact with
 * the device.
 */
    struct bt_pyrinas_client_handles
    {

        /** Handle of the PYRINAS data characteristic, as provided by
	 *  a discovery.
         */
        uint16_t data;

        /** Handle of the CCC descriptor of the PYRINAS data characteristic,
	 *  as provided by a discovery.
         */
        uint16_t data_ccc;
    };

    /** @brief PYRINAS Client callback structure. */
    struct bt_pyrinas_client_cb
    {
        /** @brief Data received callback.
	 *
	 * The data has been received as a notification of the PYRINAS Data
	 * Characteristic.
	 *
	 * @param[in] data Received data.
	 * @param[in] len Length of received data.
	 *
	 * @retval BT_GATT_ITER_CONTINUE To keep notifications enabled.
	 * @retval BT_GATT_ITER_STOP To disable notifications.
	 */
        uint8_t (*received)(const uint8_t *data, uint16_t len);

        /** @brief Data notifications disabled callback.
	 *
	 * Data notifications have been disabled.
	 */
        void (*unsubscribed)(void);
    };

    /** @brief PYRINAS Client structure. */
    struct bt_pyrinas_client
    {

        /** Connection object. */
        struct bt_conn *conn;

        /** Internal state. */
        atomic_t state;

        /** Handles on the connected peer device that are needed
         * to interact with the device.
         */
        struct bt_pyrinas_client_handles handles;

        /** GATT subscribe parameters for PYRINAS data Characteristic. */
        struct bt_gatt_subscribe_params notif_params;

        /** Application callbacks. */
        struct bt_pyrinas_client_cb cb;
    };

    /** @brief PYRINAS Client initialization structure. */
    struct bt_pyrinas_client_init_param
    {

        /** Callbacks provided by the user. */
        struct bt_pyrinas_client_cb cb;
    };

    /** @brief Initialize the PYRINAS Client module.
 *
 * This function initializes the PYRINAS Client module with callbacks provided by
 * the user.
 *
 * @param[in,out] pyrinas    PYRINAS Client instance.
 * @param[in] init_param PYRINAS Client initialization parameters.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a negative error code is returned.
 */
    int bt_pyrinas_client_init(struct bt_pyrinas_client *pyrinas,
                               const struct bt_pyrinas_client_init_param *init_param);

    /** @brief Send data to the server.
 *
 * This function writes to the RX Characteristic of the server.
 *
 * @note This procedure is asynchronous. Therefore, the data to be sent must
 * remain valid while the function is active.
 *
 * @param[in,out] pyrinas PYRINAS Client instance.
 * @param[in] data Data to be transmitted.
 * @param[in] len Length of data.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a negative error code is returned.
 */
    int bt_pyrinas_client_send(struct bt_pyrinas_client *pyrinas, const uint8_t *data,
                               uint16_t len);

    /** @brief Assign handles to the PYRINAS Client instance.
 *
 * This function should be called when a link with a peer has been established
 * to associate the link to this instance of the module. This makes it
 * possible to handle several links and associate each link to a particular
 * instance of this module. The GATT attribute handles are provided by the
 * GATT DB discovery module.
 *
 * @param[in] dm Discovery object.
 * @param[in,out] pyrinas PYRINAS Client instance.
 *
 * @retval 0 If the operation was successful.
 * @retval (-ENOTSUP) Special error code used when UUID
 *         of the service does not match the expected UUID.
 * @retval Otherwise, a negative error code is returned.
 */
    int bt_pyrinas_handles_assign(struct bt_gatt_dm *dm,
                                  struct bt_pyrinas_client *pyrinas);

    /** @brief Request the peer to start sending notifications for the Data
 *	   Characteristic.
 *
 * This function enables notifications for the PYRINAS Data Characteristic at the peer
 * by writing to the CCC descriptor of the PYRINAS Data Characteristic.
 *
 * @param[in,out] pyrinas PYRINAS Client instance.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a negative error code is returned.
 */
    int bt_pyrinas_subscribe_receive(struct bt_pyrinas_client *pyrinas);

    /* Check if busy.. */
    bool bt_pyrinas_client_is_busy(struct bt_pyrinas_client *pyrinas_c);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* BT_PYRINAS_CLIENT_H_ */

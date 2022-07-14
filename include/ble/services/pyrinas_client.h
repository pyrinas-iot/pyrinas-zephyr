/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BT_PYRINAS_CLIENT_H_
#define BT_PYRINAS_CLIENT_H_

#include <bluetooth/gatt.h>
#include <bluetooth/conn.h>

struct bt_pyrinas_client_handles
{
    uint16_t data;
    uint16_t data_ccc;
};

struct bt_pyrinas_client_cb
{
    uint8_t (*received)(const uint8_t *data, uint16_t len);
    void (*unsubscribed)(void);
};

struct bt_pyrinas_client
{

    struct bt_conn *conn;
    atomic_t state;
    struct bt_pyrinas_client_handles handles;
    struct bt_gatt_subscribe_params notif_params;
    struct bt_pyrinas_client_cb cb;
};

struct bt_pyrinas_client_init_param
{
    struct bt_pyrinas_client_cb cb;
};

int bt_pyrinas_client_init(struct bt_pyrinas_client *pyrinas,
                           const struct bt_pyrinas_client_init_param *init_param);

int bt_pyrinas_client_send(struct bt_pyrinas_client *pyrinas, const uint8_t *data,
                           uint16_t len);

int bt_pyrinas_subscribe(struct bt_pyrinas_client *pyrinas);

bool bt_pyrinas_client_is_busy(struct bt_pyrinas_client *pyrinas_c);

void bt_pyrinas_client_reset(struct bt_pyrinas_client *pyrinas_c);

#endif /* BT_PYRINAS_CLIENT_H_ */

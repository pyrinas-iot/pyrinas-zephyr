/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#include <ble/services/pyrinas_client.h>
#include <ble/services/pyrinas.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_c);

enum
{
    PYRINAS_C_INITIALIZED,
    PYRINAS_C_NOTIF_ENABLED,
    PYRINAS_C_WRITE_PENDING
};

static uint8_t on_received(struct bt_conn *conn,
                           struct bt_gatt_subscribe_params *params,
                           const void *data, uint16_t length)
{
    struct bt_pyrinas_client *pyrinas;

    /* Retrieve PYRINAS Client module context. */
    pyrinas = CONTAINER_OF(params, struct bt_pyrinas_client, notif_params);

    if (!data)
    {
        LOG_DBG("[UNSUBSCRIBED]");
        params->value_handle = 0;
        atomic_clear_bit(&pyrinas->state, PYRINAS_C_NOTIF_ENABLED);
        if (pyrinas->cb.unsubscribed)
        {
            pyrinas->cb.unsubscribed();
        }
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[NOTIFICATION] data %p length %u", data, length);
    if (pyrinas->cb.received)
    {
        return pyrinas->cb.received(data, length);
    }

    return BT_GATT_ITER_CONTINUE;
}

static void on_sent(struct bt_conn *conn, void *user_data)
{
    struct bt_pyrinas_client *pyrinas_c = user_data;

    /* Clear the state */
    atomic_clear_bit(&pyrinas_c->state, PYRINAS_C_WRITE_PENDING);
}

int bt_pyrinas_client_init(struct bt_pyrinas_client *pyrinas_c,
                           const struct bt_pyrinas_client_init_param *pyrinas_c_init)
{
    if (!pyrinas_c || !pyrinas_c_init)
    {
        return -EINVAL;
    }

    if (atomic_test_and_set_bit(&pyrinas_c->state, PYRINAS_C_INITIALIZED))
    {
        return -EALREADY;
    }

    memcpy(&pyrinas_c->cb, &pyrinas_c_init->cb, sizeof(pyrinas_c->cb));

    return 0;
}

bool bt_pyrinas_client_is_busy(struct bt_pyrinas_client *pyrinas_c)
{
    return atomic_test_bit(&pyrinas_c->state, PYRINAS_C_WRITE_PENDING);
}

int bt_pyrinas_client_send(struct bt_pyrinas_client *pyrinas_c, const uint8_t *data,
                           uint16_t len)
{
    int err;

    if (!pyrinas_c->conn)
    {
        return -ENOTCONN;
    }

    if (atomic_test_and_set_bit(&pyrinas_c->state, PYRINAS_C_WRITE_PENDING))
    {
        return -EALREADY;
    }

    /* Don't care about responses */
    err = bt_gatt_write_without_response_cb(pyrinas_c->conn,
                                            pyrinas_c->handles.data, data,
                                            len, false, on_sent,
                                            pyrinas_c);
    if (err)
    {
        atomic_clear_bit(&pyrinas_c->state, PYRINAS_C_WRITE_PENDING);

        LOG_ERR("Unable to write without rsp. Code: %i", err);
    }

    return err;
}

int bt_pyrinas_subscribe(struct bt_pyrinas_client *pyrinas_c)
{
    int err;

    if (atomic_test_and_set_bit(&pyrinas_c->state, PYRINAS_C_NOTIF_ENABLED))
    {
        return -EALREADY;
    }

    pyrinas_c->notif_params.notify = on_received;
    pyrinas_c->notif_params.value = BT_GATT_CCC_NOTIFY;
    pyrinas_c->notif_params.value_handle = pyrinas_c->handles.data;
    pyrinas_c->notif_params.ccc_handle = pyrinas_c->handles.data_ccc;
    atomic_set_bit(pyrinas_c->notif_params.flags,
                   BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

    err = bt_gatt_subscribe(pyrinas_c->conn, &pyrinas_c->notif_params);
    if (err)
    {
        LOG_ERR("Subscribe failed (err %d)", err);
        atomic_clear_bit(&pyrinas_c->state, PYRINAS_C_NOTIF_ENABLED);
    }
    else
    {
        LOG_INF("Subscribe success!");
    }

    return err;
}

void bt_pyrinas_client_reset(struct bt_pyrinas_client *pyrinas_c)
{
    /* Reset state */
    atomic_clear_bit(&pyrinas_c->state, PYRINAS_C_WRITE_PENDING);
    atomic_clear_bit(&pyrinas_c->state, PYRINAS_C_NOTIF_ENABLED);
}
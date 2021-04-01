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

static void on_sent(struct bt_conn *conn, uint8_t err,
                    struct bt_gatt_write_params *params)
{
    struct bt_pyrinas_client *pyrinas_c;
    const void *data;
    uint16_t length;

    /* Retrieve PYRINAS Client module context. */
    pyrinas_c = CONTAINER_OF(params, struct bt_pyrinas_client, write_params);

    /* Make a copy of volatile data that is required by the callback. */
    data = params->data;
    length = params->length;

    atomic_clear_bit(&pyrinas_c->state, PYRINAS_C_WRITE_PENDING);
    if (pyrinas_c->cb.sent)
    {
        pyrinas_c->cb.sent(conn, err, data, length);
    }
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

    pyrinas_c->write_params.func = on_sent;
    pyrinas_c->write_params.handle = pyrinas_c->handles.data;
    pyrinas_c->write_params.offset = 0;
    pyrinas_c->write_params.data = data;
    pyrinas_c->write_params.length = len;

    err = bt_gatt_write(pyrinas_c->conn, &pyrinas_c->write_params);
    if (err)
    {
        atomic_clear_bit(&pyrinas_c->state, PYRINAS_C_WRITE_PENDING);
    }

    return err;
}

int bt_pyrinas_handles_assign(struct bt_gatt_dm *dm,
                              struct bt_pyrinas_client *pyrinas_c)
{
    const struct bt_gatt_dm_attr *gatt_service_attr =
        bt_gatt_dm_service_get(dm);
    const struct bt_gatt_service_val *gatt_service =
        bt_gatt_dm_attr_service_val(gatt_service_attr);
    const struct bt_gatt_dm_attr *gatt_chrc;
    const struct bt_gatt_dm_attr *gatt_desc;

    if (bt_uuid_cmp(gatt_service->uuid, BT_UUID_PYRINAS_SERVICE))
    {
        return -ENOTSUP;
    }
    LOG_DBG("Getting handles from PYRINAS service.");
    memset(&pyrinas_c->handles, 0xFF, sizeof(pyrinas_c->handles));

    /* PYRINAS Data Characteristic */
    gatt_chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_PYRINAS_DATA);
    if (!gatt_chrc)
    {
        LOG_ERR("Missing PYRINAS Data characteristic.");
        return -EINVAL;
    }
    /* PYRINAS TX */
    gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_PYRINAS_DATA);
    if (!gatt_desc)
    {
        LOG_ERR("Missing PYRINAS Data value descriptor in characteristic.");
        return -EINVAL;
    }
    LOG_DBG("Found handle for PYRINAS Data characteristic.");
    pyrinas_c->handles.data = gatt_desc->handle;
    /* PYRINAS TX CCC */
    gatt_desc = bt_gatt_dm_desc_by_uuid(dm, gatt_chrc, BT_UUID_GATT_CCC);
    if (!gatt_desc)
    {
        LOG_ERR("Missing PYRINAS Data CCC in characteristic.");
        return -EINVAL;
    }
    LOG_DBG("Found handle for CCC of PYRINAS TX characteristic.");
    pyrinas_c->handles.data_ccc = gatt_desc->handle;

    /* Assign connection instance. */
    pyrinas_c->conn = bt_gatt_dm_conn_get(dm);
    return 0;
}

int bt_pyrinas_subscribe_receive(struct bt_pyrinas_client *pyrinas_c)
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
        LOG_DBG("[SUBSCRIBED]");
    }

    return err;
}

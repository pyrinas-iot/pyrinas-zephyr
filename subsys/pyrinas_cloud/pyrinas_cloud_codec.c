#include <zephyr.h>
#include <stdio.h>
#include <pyrinas_cloud/pyrinas_cloud.h>
#include <cellular/cellular.h>

#include "pyrinas_cloud_codec.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_cloud_codec);

QCBORError encode_ota_request(enum pyrinas_cloud_ota_cmd_type cmd_type, uint8_t *p_buf, size_t data_len, size_t *payload_len)
{

    /* Setup of the goods */
    UsefulBuf buf = {
        .ptr = p_buf,
        .len = data_len};
    QCBOREncodeContext ec;
    QCBOREncode_Init(&ec, buf);

    /* Create over-arching map */
    QCBOREncode_OpenMap(&ec);
    QCBOREncode_AddUInt64ToMapN(&ec, 0, cmd_type);
    QCBOREncode_CloseMap(&ec);

    /* Finish and get size */
    return QCBOREncode_FinishGetSize(&ec, payload_len);
}

int decode_ota_data(struct pyrinas_cloud_ota_data *ota_data, const char *data, size_t data_len)
{

    /* Setup of the goods */
    UsefulBufC buf = {
        .ptr = data,
        .len = data_len};
    QCBORDecodeContext dc;
    QCBORItem item;
    QCBORDecode_Init(&dc, buf, QCBOR_DECODE_MODE_NORMAL);

    QCBORDecode_GetNext(&dc, &item);
    if (item.uDataType != QCBOR_TYPE_MAP)
    {
        LOG_ERR("Expected CBOR map structure.");
        return -ENOEXEC;
    }

    /* Get the version */
    QCBORDecode_GetNext(&dc, &item);
    if (item.val.string.len > sizeof(ota_data->version))
        return -ENOEXEC;
    memcpy(ota_data->version, item.val.string.ptr, item.val.string.len);

    /* Get the host */
    QCBORDecode_GetNext(&dc, &item);
    if (item.val.string.len > sizeof(ota_data->host))
        return -ENOEXEC;
    memcpy(ota_data->host, item.val.string.ptr, item.val.string.len);

    /* Get the file */
    QCBORDecode_GetNext(&dc, &item);
    if (item.val.string.len > sizeof(ota_data->file))
        return -ENOEXEC;
    memcpy(ota_data->file, item.val.string.ptr, item.val.string.len);

    /* Get the force value */
    QCBORDecode_GetNext(&dc, &item);
    ota_data->force = item.uDataType == QCBOR_TYPE_TRUE;

    printk("version:%s, url:%s/%s, force:%d\n", ota_data->version, ota_data->host, ota_data->file, ota_data->force);

    return 0;
}

QCBORError encode_telemetry_data(struct pyrinas_cloud_telemetry_data *p_data, uint8_t *p_buf, size_t data_len, size_t *payload_len)
{
    /* Setup of the goods */
    UsefulBuf buf = {
        .ptr = p_buf,
        .len = data_len};
    QCBOREncodeContext ec;
    QCBOREncode_Init(&ec, buf);

    /* Create over-arching map */
    QCBOREncode_OpenMap(&ec);
    QCBOREncode_AddSZStringToMapN(&ec, tel_type_version, p_data->version);

    /* Add RSRP if valid*/
    if (p_data->rsrp <= RSRP_THRESHOLD)
    {
        QCBOREncode_AddUInt64ToMapN(&ec, tel_type_rsrp, p_data->rsrp);
    }

    QCBOREncode_CloseMap(&ec);

    /* Finish and get size */
    return QCBOREncode_FinishGetSize(&ec, payload_len);
}
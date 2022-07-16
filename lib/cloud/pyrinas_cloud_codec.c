/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <stdio.h>
#include <pyrinas_cloud/pyrinas_cloud.h>
#include <pyrinas_cloud/pyrinas_cloud_codec.h>
#include <qcbor/qcbor_spiffy_decode.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_cloud_codec);

QCBORError encode_ota_request(struct pyrinas_cloud_ota_request *req, uint8_t *p_buf, size_t data_len, size_t *payload_len)
{

    /* Setup of the goods */
    UsefulBuf buf = {
        .ptr = p_buf,
        .len = data_len};
    QCBOREncodeContext ec;
    QCBOREncode_Init(&ec, buf);

    /* Create over-arching map */
    QCBOREncode_OpenMap(&ec);
    QCBOREncode_AddUInt64ToMapN(&ec, ota_req_type_pos, req->type);

    if (req->type == ota_cmd_type_download_bytes)
    {
        /* Image name */
        QCBOREncode_AddSZStringToMapN(&ec, ota_req_id_pos, req->id);

        /* Start and end */
        QCBOREncode_AddUInt64ToMapN(&ec, ota_req_start_pos, req->start_pos);
        QCBOREncode_AddUInt64ToMapN(&ec, ota_req_end_pos, req->end_pos);
    }

    QCBOREncode_CloseMap(&ec);

    /* Finish and get size */
    return QCBOREncode_FinishGetSize(&ec, payload_len);
}

void decode_ota_version(union pyrinas_cloud_ota_version *p_version, QCBORDecodeContext *dc, int64_t label)
{

    QCBORDecode_EnterMapFromMapN(dc, label);
    uint64_t temp;

    QCBORDecode_GetUInt64ConvertAllInMapN(dc, major_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    p_version->major = temp;
    LOG_DBG("%d", (uint8_t)temp);

    QCBORDecode_GetUInt64ConvertAllInMapN(dc, minor_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    p_version->minor = temp;
    LOG_DBG("%d", (uint8_t)temp);

    QCBORDecode_GetUInt64ConvertAllInMapN(dc, patch_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    p_version->patch = temp;
    LOG_DBG("%d", (uint8_t)temp);

    QCBORDecode_GetUInt64ConvertAllInMapN(dc, commit_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    p_version->commit = temp;
    LOG_DBG("%d", (uint8_t)temp);

    /* Get the hash from array */
    QCBORDecode_EnterArrayFromMapN(dc, hash_pos);
    for (int64_t i = 0; i < sizeof(p_version->hash); i++)
    {
        QCBORDecode_GetUInt64(dc, &temp);
        p_version->hash[i] = temp;
    }
    QCBORDecode_ExitArray(dc);

    uint8_t buf[64];
    snprintf(buf, sizeof(buf), "%d.%d.%d-%d-%.*s", p_version->major, p_version->minor, p_version->patch, p_version->commit, sizeof(p_version->hash), p_version->hash);
    LOG_DBG("Version: %s", buf);

    QCBORDecode_ExitMap(dc);
}

QCBORError decode_ota_download(struct pyrinas_cloud_ota_download *ota_download, const char *data, size_t data_len)
{

    LOG_HEXDUMP_DBG(data, data_len, "ota download:");

    /* Setup of the goods */
    QCBORError uErr;
    UsefulBufC buf = {
        .ptr = data,
        .len = data_len};
    QCBORDecodeContext dc;
    QCBORDecode_Init(&dc, buf, QCBOR_DECODE_MODE_NORMAL);
    QCBORDecode_EnterMap(&dc, NULL);

    // Check to make sure we have a map
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    uint64_t temp;
    QCBORDecode_GetUInt64ConvertAllInMapN(&dc, ota_download_position_start_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    ota_download->start_pos = (uint32_t)temp;

    // Check to make sure we have a map
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    QCBORDecode_GetUInt64ConvertAllInMapN(&dc, ota_download_position_end_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    ota_download->end_pos = (uint32_t)temp;

    // Check error
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    /* Get the file */
    UsefulBufC data_buf;
    QCBORDecode_GetByteStringInMapN(&dc, ota_download_position_data_pos, &data_buf);

    // Check error
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    /* Copy if no error*/
    memcpy(ota_download->data, data_buf.ptr, MIN(data_buf.len, sizeof(ota_download->data)));

    QCBORDecode_GetUInt64ConvertAllInMapN(&dc, ota_download_position_len_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    ota_download->len = (uint32_t)temp;

    // Check error
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    /* Exit main map and return*/
    QCBORDecode_ExitMap(&dc);

Done:

    return QCBORDecode_Finish(&dc);
}

QCBORError decode_ota_package(struct pyrinas_cloud_ota_package *ota_package, const char *data, size_t data_len)
{
    LOG_HEXDUMP_DBG(data, data_len, "ota package:");

    /* Setup of the goods */
    QCBORError uErr;
    UsefulBufC buf = {
        .ptr = data,
        .len = data_len};
    QCBORDecodeContext dc;
    QCBORDecode_Init(&dc, buf, QCBOR_DECODE_MODE_NORMAL);
    QCBORDecode_EnterMap(&dc, NULL);

    // Check to make sure we have a map
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    /* Get the host */
    UsefulBufC id_data;
    QCBORDecode_GetTextStringInMapN(&dc, pyrinas_cloud_ota_package_id_pos, &id_data);

    /* Make sure we had the ID */
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    /*Copy if no error*/
    memcpy(ota_package->id, id_data.ptr, MIN(id_data.len, sizeof(ota_package->id)));

    /* Get the ota version struct */
    decode_ota_version(&ota_package->version, &dc, pyrinas_cloud_ota_package_version_pos);

    // Check to make sure we have a map
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    uint64_t temp;
    QCBORDecode_GetUInt64ConvertAllInMapN(&dc, pyrinas_cloud_ota_package_size_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    ota_package->size = (size_t)temp;

    // Check error
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    /* Exit main map and return*/
    QCBORDecode_ExitMap(&dc);

Done:

    return QCBORDecode_Finish(&dc);
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

    /*Add versio if it's there*/
    if (p_data->has_version)
    {
        QCBOREncode_AddSZStringToMapN(&ec, tel_type_version, p_data->version);
    }

    /* Add RSRP if valid*/
    if (p_data->has_rsrp)
    {
        QCBOREncode_AddUInt64ToMapN(&ec, tel_type_rsrp, p_data->rsrp);
    }

    /* Add Hub RSSI if valid*/
    if (p_data->has_central_rssi)
    {
        QCBOREncode_AddInt64ToMapN(&ec, tel_type_rssi_central, p_data->central_rssi);
    }

    /* Add Cabinet RSSI if valid*/
    if (p_data->has_peripheral_rssi)
    {
        QCBOREncode_AddInt64ToMapN(&ec, tel_type_rssi_peripheral, p_data->peripheral_rssi);
    }

    QCBOREncode_CloseMap(&ec);

    /* Finish and get size */
    return QCBOREncode_FinishGetSize(&ec, payload_len);
}

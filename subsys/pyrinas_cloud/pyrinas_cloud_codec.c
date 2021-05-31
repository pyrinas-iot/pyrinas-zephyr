/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <stdio.h>
#include <pyrinas_cloud/pyrinas_cloud.h>
#include <cellular/cellular.h>
#include <qcbor/qcbor_spiffy_decode.h>

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
    LOG_INF("Version: %s", buf);

    QCBORDecode_ExitMap(dc);
}

void decode_ota_file_info(struct pyrinas_cloud_ota_file_info *p_file_info, QCBORDecodeContext *dc)
{
    QCBORError uErr;
    uint64_t temp;

    QCBORDecode_EnterMapFromMapN(dc, 0);

    //Get the image type
    QCBORDecode_GetUInt64ConvertAllInMapN(dc, image_type_pos, QCBOR_CONVERT_TYPE_XINT64, &temp);
    p_file_info->image_type = (uint8_t)temp;

    /* Get the host */
    UsefulBufC host_data;
    QCBORDecode_GetTextStringInMapN(&dc, host_pos, &host_data);
    memcpy(p_file_info->host, host_data.ptr, host_data.len);

    // Check to make sure we have a map
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        return;
    }

    /* Get the file */
    UsefulBufC file_data;
    QCBORDecode_GetTextStringInMapN(&dc, file_pos, &file_data);
    memcpy(p_file_info->file, file_data.ptr, file_data.len);

    // Check to make sure we have a map
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        return;
    }

    LOG_INF("TYPE: %d URL: %s/%s", p_file_info->image_type, p_file_info->host, p_file_info->file);

    QCBORDecode_ExitMap(dc);
}

QCBORError decode_ota_package(struct pyrinas_cloud_ota_package *ota_package, const char *data, size_t data_len)
{
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

    /* Get the ota version struct */
    decode_ota_version(&ota_package->version, &dc, version_pos);

    // Check to make sure we have a map
    uErr = QCBORDecode_GetError(&dc);
    if (uErr != QCBOR_SUCCESS)
    {
        goto Done;
    }

    /* Get the hash from array */
    QCBORDecode_EnterArrayFromMapN(&dc, file_info_pos);
    for (uint8_t i = 0; i < PYRINAS_OTA_PACKAGE_MAX_FILE_COUNT; i++)
    {
        decode_ota_file_info(&ota_package->files[i], &dc);
    }
    QCBORDecode_ExitArray(&dc);

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
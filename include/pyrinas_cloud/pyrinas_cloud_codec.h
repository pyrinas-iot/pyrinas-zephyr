/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _PYRINAS_CLOUD_CBOR_PARSER_H
#define _PYRINAS_CLOUD_CBOR_PARSER_H

#include <zephyr.h>
#include <qcbor/qcbor.h>
#include <pyrinas_cloud/pyrinas_cloud.h>
#include <pyrinas_cloud/pyrinas_cloud_ota.h>

enum pyrinas_cloud_ota_request_position
{
	ota_req_type_pos,
	ota_req_id_pos,
	ota_req_start_pos,
	ota_req_end_pos,
};

enum pyrinas_cloud_ota_download_positions
{
	ota_download_position_start_pos,
	ota_download_position_end_pos,
	ota_download_position_data_pos,
	ota_download_position_len_pos
};

typedef enum
{
	major_pos,
	minor_pos,
	patch_pos,
	commit_pos,
	hash_pos,
} pyrinas_cloud_ota_version_pos_t;

typedef enum
{
	image_type_pos,
	host_pos,
	file_pos,
} pyrinas_cloud_ota_file_info_pos_t;

typedef enum
{
	pyrinas_cloud_ota_package_id_pos,
	pyrinas_cloud_ota_package_version_pos,
	pyrinas_cloud_ota_package_file_data_pos,
	pyrinas_cloud_ota_package_size_pos,
	pyrinas_cloud_ota_package_date_pos,
} pyrinas_cloud_ota_package_pos_t;

/**
 * @brief Encodes OTA request
 *
 * @param cmd_type different OTA requests depending on the state
 * @param buf provided buffer data will be encoded to
 * @param data_len size of provided buffer
 * @param payload_len pointer to size_t variable which tracks the final encoded size
 * @return QCBORError
 */
QCBORError encode_ota_request(struct pyrinas_cloud_ota_request *req, uint8_t *p_buf, size_t data_len, size_t *payload_len);

/**
 * @brief Decode OTT package
 *
 * @param ota_data strutured data that the data will be decoded to
 * @param data raw bytes recieved that need decoding
 * @param data_len size of the raw buffer
 * @return QCBORError
 */
QCBORError decode_ota_package(struct pyrinas_cloud_ota_package *ota_data, const char *data, size_t data_len);

QCBORError decode_ota_download(struct pyrinas_cloud_ota_download *ota_download, const char *data, size_t data_len);

/**
 * @brief Encode telemetry data for device
 *
 * @param p_data raw structured data from the application
 * @param p_buf buffer to encode the data to
 * @param data_len size of the provided buffer (p_buf)
 * @param payload_len pointer to a size_t which will take the final payload size
 * @return QCBORError
 */
QCBORError encode_telemetry_data(struct pyrinas_cloud_telemetry_data *p_data, uint8_t *buf, size_t data_len, size_t *payload_len);

#endif /* _PYRINAS_CLOUD_CBOR_PARSER_H */

#ifndef _PYRINAS_CLOUD_CBOR_PARSER_H
#define _PYRINAS_CLOUD_CBOR_PARSER_H

#include <zephyr.h>
#include <qcbor.h>
#include <pyrinas_cloud/pyrinas_cloud.h>

QCBORError encode_ota_request(enum pyrinas_cloud_ota_cmd_type cmd_type, uint8_t *buf, size_t data_len, size_t *payload_len);
int decode_ota_data(struct pyrinas_cloud_ota_data *ota_data, const char *data, size_t data_len);
QCBORError encode_telemetry_data(struct pyrinas_cloud_telemetry_data *p_data, uint8_t *buf, size_t data_len, size_t *payload_len);

#endif /* _PYRINAS_CLOUD_CBOR_PARSER_H */
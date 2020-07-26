#ifndef _PYRINAS_CLOUD_CBOR_PARSER_H
#define _PYRINAS_CLOUD_CBOR_PARSER_H

#include <zephyr.h>
#include <pyrinas_cloud/pyrinas_cloud.h>

int decode_ota_data(struct pyrinas_cloud_ota_data *ota_data, const char *data, size_t data_len);
int encode_telemetry_data(uint8_t *buf, size_t data_len, size_t *payload_len);

#endif /* _PYRINAS_CLOUD_CBOR_PARSER_H */
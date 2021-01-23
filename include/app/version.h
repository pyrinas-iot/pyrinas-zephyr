#ifndef PYRINAS_VERSION_H
#define PYRINAS_VERSION_H

#include <pyrinas_cloud/pyrinas_cloud.h>

extern const union pyrinas_cloud_ota_version pyrinas_version;

int get_version_string(uint8_t *p_buf, const size_t size);

#endif /*PYRINAS_VERSION_H*/
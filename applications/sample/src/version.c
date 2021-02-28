#include <stdio.h>
#include <app/version.h>

const union pyrinas_cloud_ota_version pyrinas_version = {
    .major = PYRINAS_APP_VERSION_MAJOR,
    .minor = PYRINAS_APP_VERSION_MINOR,
    .patch = PYRINAS_APP_VERSION_PATCH,
    .commit = PYRINAS_APP_VERSION_COMMIT,
    .hash = STRINGIFY(PYRINAS_APP_VERSION_HASH),
};

int get_version_string(uint8_t *p_buf, const size_t size)
{
    return snprintf(p_buf, size, "%d.%d.%d-%d-%.*s", pyrinas_version.major, pyrinas_version.minor, pyrinas_version.patch, pyrinas_version.commit, sizeof(pyrinas_version.hash), pyrinas_version.hash);
}
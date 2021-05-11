#include <stdio.h>
#include <pyrinas_cloud/pyrinas_cloud.h>

/* Error if these are not generated.. */
#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)

const union pyrinas_cloud_ota_version pyrinas_version = {
    .major = CONFIG_PYRINAS_APP_VERSION_MAJOR,
    .minor = CONFIG_PYRINAS_APP_VERSION_MINOR,
    .patch = CONFIG_PYRINAS_APP_VERSION_PATCH,
    .commit = CONFIG_PYRINAS_APP_VERSION_COMMIT,
    .hash = STRINGIFY(CONFIG_PYRINAS_APP_VERSION_HASH),
};

int get_version_string(uint8_t *p_buf, const size_t size)
{
    return snprintf(p_buf, size, "%d.%d.%d-%d-%.*s", pyrinas_version.major, pyrinas_version.minor, pyrinas_version.patch, pyrinas_version.commit, sizeof(pyrinas_version.hash), pyrinas_version.hash);
}

#endif
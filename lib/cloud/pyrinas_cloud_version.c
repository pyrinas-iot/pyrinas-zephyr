#include <stdio.h>
#include <pyrinas_cloud/pyrinas_cloud.h>

/* Error if these are not generated.. */
#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)

const union pyrinas_cloud_ota_version pyrinas_version = {
    .major = CONFIG_PYRINAS_APP_VERSION_MAJOR,
    .minor = CONFIG_PYRINAS_APP_VERSION_MINOR,
    .patch = CONFIG_PYRINAS_APP_VERSION_PATCH,
    .commit = CONFIG_PYRINAS_APP_VERSION_COMMIT,
    .hash = CONFIG_PYRINAS_APP_VERSION_HASH,
};

#endif
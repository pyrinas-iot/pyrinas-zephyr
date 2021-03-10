/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>

/* Certificate for `LetsEncryptCA` */
static const char pyrinas_ota_primary_cert[] = {
#include "trustid-x3-root"
};

static const char pyrinas_ota_secondary_cert[] = {
#include "isrg-root-x1"
};

BUILD_ASSERT(sizeof(pyrinas_ota_primary_cert) < KB(4), "Primary certificate too large");
BUILD_ASSERT(sizeof(pyrinas_ota_primary_cert) < KB(4), "Secondary certificate too large");

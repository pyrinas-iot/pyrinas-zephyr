
/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef BT_PYRINAS_H_
#define BT_PYRINAS_H_

/**
 * @file
 * @defgroup pyrinas Pyrinas (PYRINAS) GATT Service
 * @{
 * @brief Pyrinas (PYRINAS) GATT Service API.
 */

#include <zephyr/types.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * Service UUOD String: 0000f510-8d45-6280-f64d-ff5fb31a0972
 **/

/** @brief UUID of the PYRINAS Service. **/
#define BT_UUID_PYRINAS_VAL \
    BT_UUID_128_ENCODE(0x0000f510, 0x8d45, 0x6280, 0xf64d, 0xff5fb31a0972)

/** @brief UUID of the DATA Characteristic. **/
/*#define BT_UUID_PYRINAS_DATA_VAL \
    BT_UUID_128_ENCODE(0x0000f511, 0x8d45, 0x6280, 0xf64d, 0xff5fb31a0972)*/
#define BT_UUID_PYRINAS_DATA_VAL 0xf511

#define BT_UUID_PYRINAS_SERVICE BT_UUID_DECLARE_128(BT_UUID_PYRINAS_VAL)
#define BT_UUID_PYRINAS_DATA BT_UUID_DECLARE_16(BT_UUID_PYRINAS_DATA_VAL)

#ifdef __cplusplus
}
#endif

/**
 *@}
 */

#endif /* BT_PYRINAS_H_ */
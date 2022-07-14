/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BT_PYRINAS_H_
#define BT_PYRINAS_H_

#include <zephyr/types.h>
#include <bluetooth/conn.h>
#include <bluetooth/uuid.h>
#include <bluetooth/gatt.h>

/*
 * Service UUOD String: 0000f510-8d45-6280-f64d-ff5fb31a0972
 **/

#define BT_UUID_PYRINAS_VAL \
	BT_UUID_128_ENCODE(0x0000f510, 0x8d45, 0x6280, 0xf64d, 0xff5fb31a0972)

#define BT_UUID_PYRINAS_DATA_VAL 0xf511

#define BT_UUID_PYRINAS_SERVICE BT_UUID_DECLARE_128(BT_UUID_PYRINAS_VAL)
#define BT_UUID_PYRINAS_DATA BT_UUID_DECLARE_16(BT_UUID_PYRINAS_DATA_VAL)

#endif /* BT_PYRINAS_H_ */

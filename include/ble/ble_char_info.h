/*
 * Copyright (c) 202 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BLE_CHAR_INFO_H__
#define BLE_CHAR_INFO_H__

/* Pyrinas service UUID */
#define BT_UUID_PYRINAS_S                                             \
  BT_UUID_DECLARE_128(0x72, 0x09, 0x1a, 0xb3, 0x5f, 0xff, 0x4d, 0xf6, \
                      0x80, 0x62, 0x45, 0x8d, 0xf5, 0x10, 0x00, 0x00)

/* Thingy characteristic UUID */
#define BT_UUID_PYRINAS_DATA                                          \
  BT_UUID_DECLARE_128(0x72, 0x09, 0x1a, 0xb3, 0x5f, 0xff, 0x4d, 0xf6, \
                      0x80, 0x62, 0x45, 0x8d, 0xf5, 0x11, 0x00, 0x00)

#endif //BLE_CHAR_INFO_H__
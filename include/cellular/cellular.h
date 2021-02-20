/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _MODEM_H
#define _MODEM_H

#include <modem/modem_info.h>

#define RSRP_THRESHOLD 97

/* Connect to LTE */
void cellular_configure(void);

/* Setup information service */
int cellular_info_init(void);

/* Get information from modem */
int cellular_info_get(struct modem_param_info *p_modem_info);

/* Get cellular signal strength */
char cellular_get_signal_strength();

/* Power off/disconnect */
int cellular_off();

/* Cellular on*/
int cellular_on();

#endif /* _MODEM_H */
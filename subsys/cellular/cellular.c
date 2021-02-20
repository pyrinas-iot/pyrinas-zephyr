/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <stdio.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <cellular/cellular.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(cellular);

struct modem_param_info modem_info = {0};
static char rsrp = 0xff;

void cellular_evt(const struct lte_lc_evt *const evt)
{

  if (evt->type == LTE_LC_EVT_NW_REG_STATUS)
  {

    switch (evt->nw_reg_status)
    {

    case LTE_LC_NW_REG_NOT_REGISTERED:
      LOG_DBG("not reg");
      break;
    case LTE_LC_NW_REG_REGISTERED_HOME:
      LOG_DBG("reg home");
      break;
    case LTE_LC_NW_REG_SEARCHING:
      LOG_DBG("searching");
      break;
    case LTE_LC_NW_REG_REGISTRATION_DENIED:
      LOG_DBG("reg denied");
      break;
    case LTE_LC_NW_REG_UNKNOWN:
      LOG_DBG("reg unknown");
      break;
    case LTE_LC_NW_REG_REGISTERED_ROAMING:
      LOG_DBG("reg roam");
      break;
    case LTE_LC_NW_REG_REGISTERED_EMERGENCY:
      LOG_DBG("reg em");
      break;
    case LTE_LC_NW_REG_UICC_FAIL:
      LOG_DBG("uicc fail");
      break;
    default:
      break;
    }
  }
}

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
void cellular_configure(void)
{
#if defined(CONFIG_LTE_LINK_CONTROL)
  if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT))
  {
    /* Do nothing, modem is already turned on
		 * and connected.
		 */
  }
  else
  {
#if defined(CONFIG_LWM2M_CARRIER)
    /* Wait for the LWM2M_CARRIER to configure the modem and
		 * start the connection.
		 */
    LOG_INF("Waitng for carrier registration...");
    k_sem_take(&carrier_registered, K_FOREVER);
    LOG_INF("Registered!");
#else  /* defined(CONFIG_LWM2M_CARRIER) */
    int err;

    LOG_INF("LTE Link Connecting ...");
    err = lte_lc_init_and_connect();
    __ASSERT(err == 0, "LTE link could not be established.");
    LOG_INF("LTE Link Connected!");

    lte_lc_register_handler(cellular_evt);
#endif /* defined(CONFIG_LWM2M_CARRIER) */
  }
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */
}

int cellular_on()
{
  LOG_INF("Cellular on");
  int err = lte_lc_normal();
  if (err)
    LOG_ERR("Unable to turn cellular on. Err: %d", err);

  return err;
}

int cellular_off()
{
  /* Turn off and shutdown modem */
  LOG_INF("Cellular off");
  int err = lte_lc_power_off();
  if (err)
    LOG_ERR("Unable to turn cellular off. Err: %d", err);

  return err;
}

/**@brief Returns rsrp so it can be use elsewhere (like Pyrinas Cloud)
 */
char cellular_get_signal_strength()
{
  return rsrp;
}

static void modem_rsrp_handler(char rsrp_value)
{
  /* RSRP raw values that represent actual signal strength are
	 * 0 through 97 (per "nRF91 AT Commands" v1.1). If the received value
	 * falls outside this range, we should not send the value.
	 */
  if (rsrp_value > RSRP_THRESHOLD)
  {
    return;
  }

  LOG_DBG("[%s:%d] rsrp value %d", __func__,
          __LINE__, rsrp_value);

  /* Copy over the value */
  rsrp = rsrp_value;
}

int cellular_info_init()
{

  int err;

  err = modem_info_init();
  if (err)
  {
    LOG_ERR("Could not initialize modem info module. Err %i", err);
    return err;
  }

  err = modem_info_params_init(&modem_info);
  if (err)
  {
    LOG_ERR("Could not initialize modem info parameters. Err %i", err);
    return err;
  }

  /* Register the callback to get the signal strength */
  err = modem_info_rsrp_register(modem_rsrp_handler);
  if (err)
  {
    LOG_ERR("Could not subscribe to rsrp events. Err: %i", err);
    return err;
  }

  return 0;
}

int cellular_info_get(struct modem_param_info *p_modem_info)
{

  int err;

  err = modem_info_params_get(&modem_info);
  if (err)
  {
    LOG_ERR("Could not obtain cell information");
    return err;
  }

  /* Set the pointer to the current modem_info struct */
  p_modem_info = &modem_info;

  return 0;
}
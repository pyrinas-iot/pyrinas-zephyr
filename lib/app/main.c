/*
 * Copyright (c) 2020 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/gpio.h>
#include <settings/settings.h>
#include <drivers/counter.h>
#include <ble/ble_m.h>

#if defined(CONFIG_NRF_MODEM_LIB)
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/at_cmd.h>
#include <modem/at_notif.h>

#include <dfu/mcuboot.h>
#endif

#include <power/reboot.h>
#include <app/app.h>
#include <worker/worker.h>
#include <ota/cert.h>

#if defined(CONFIG_PYRINAS_CLOUD_PROVISION_CERTIFICATES)
#include <modem/modem_key_mgmt.h>
#endif

#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)
#include <pyrinas_cloud/pyrinas_cloud.h>
#endif

#include <cellular/cellular.h>

#ifdef CONFIG_MCUMGR_CMD_IMG_MGMT
#include "img_mgmt/img_mgmt.h"
#endif

#include <logging/log.h>
LOG_MODULE_REGISTER(main);

#define GPIO0 DT_LABEL(DT_NODELABEL(gpio0))
struct device *gpio;

static K_SEM_DEFINE(main_thread_proceed_sem, 0, 1);

#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)
/* Work defs */
static struct k_delayed_work reconnect_work;
#endif

#if defined(CONFIG_FILE_SYSTEM_LITTLEFS)
#include <fs/fs.h>
#include <fs/littlefs.h>
#include <storage/flash_map.h>

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
	.type = FS_LITTLEFS,
	.fs_data = &storage,
	.storage_dev = (void *)FLASH_AREA_ID(external_flash),
	.mnt_point = "/lfs",
};
#endif

static void flash_init()
{
#if defined(CONFIG_FILE_SYSTEM_LITTLEFS)
	struct fs_mount_t *mp = &lfs_storage_mnt;
	unsigned int id = (uintptr_t)mp->storage_dev;
	const struct flash_area *pfa;
	struct fs_statvfs sbuf;

	int rc;

	rc = flash_area_open(id, &pfa);
	if (rc < 0)
	{
		LOG_ERR("FAIL: unable to find flash area %u: %d",
				id, rc);
		return;
	}

	LOG_INF("Area %u at 0x%x on %s for %u bytes",
			id, (unsigned int)pfa->fa_off, pfa->fa_dev_name,
			(unsigned int)pfa->fa_size);

	/* Optional wipe flash contents */
	if (IS_ENABLED(CONFIG_APP_WIPE_STORAGE))
	{
		LOG_INF("Erasing flash area ... ");
		rc = flash_area_erase(pfa, 0, pfa->fa_size);
		LOG_INF("%d", rc);
	}

	flash_area_close(pfa);

	rc = fs_mount(mp);
	if (rc < 0)
	{
		LOG_ERR("FAIL: mount id %u at %s: %d",
				(unsigned int)mp->storage_dev, mp->mnt_point,
				rc);
		return;
	}
	LOG_INF("%s mount: %d", mp->mnt_point, rc);

	rc = fs_statvfs(mp->mnt_point, &sbuf);
	if (rc < 0)
	{
		LOG_ERR("FAIL: statvfs: %d", rc);
		return;
	}

	LOG_INF("%s: bsize = %lu ; frsize = %lu ;"
			" blocks = %lu ; bfree = %lu",
			mp->mnt_point,
			sbuf.f_bsize, sbuf.f_frsize,
			sbuf.f_blocks, sbuf.f_bfree);
#endif
}

/* RTC control */
const struct device *rtc;

#ifdef CONFIG_PCF85063A
static void rtc_init()
{

	// Get the device
	rtc = device_get_binding("PCF85063A");
	if (rtc == NULL)
	{
		LOG_ERR("Failed to get RTC device binding");
		return;
	}

	LOG_INF("device is %p, name is %s", rtc, log_strdup(rtc->name));

	// 2 seconds
	const struct counter_alarm_cfg cfg = {
		.ticks = 10,
	};

	// Set the alarm
	int ret = counter_set_channel_alarm(rtc, 0, &cfg);
	if (ret)
	{
		LOG_ERR("Unable to set alarm");
	}
}
#endif

#if defined(CONFIG_PYRINAS_CLOUD_PROVISION_CERTIFICATES)
/* Provision certificate */
int pyrinas_cloud_ota_cert_provision(void)
{
	int err;
	bool exists;
	uint8_t unused;

	err = modem_key_mgmt_exists(CONFIG_PYRINAS_CLOUD_HTTPS_SEC_TAG,
								MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
								&exists, &unused);
	if (err)
	{
		printk("Failed to check for certificates err %d\n", err);
		return err;
	}

	if (exists)
	{
		/* For the sake of simplicity we delete what is provisioned
		 * with our security tag and reprovision our certificate.
		 */
		err = modem_key_mgmt_delete(CONFIG_PYRINAS_CLOUD_HTTPS_SEC_TAG,
									MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
		if (err)
		{
			printk("Failed to delete existing certificate, err %d\n",
				   err);
		}
	}

	LOG_INF("Provisioning certificate");

	/*  Provision certificate to the modem */
	err = modem_key_mgmt_write(CONFIG_PYRINAS_CLOUD_HTTPS_SEC_TAG,
							   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
							   pyrinas_ota_primary_cert, sizeof(pyrinas_ota_primary_cert) - 1);
	if (err)
	{
		printk("Failed to provision certificate, err %d\n", err);
		return err;
	}

	return 0;
}

void pyrinas_cloud_evt_handler(const struct pyrinas_cloud_evt *const p_evt)
{

	LOG_DBG("pyrinas_cloud_evt_type: %d", p_evt->type);

	switch (p_evt->type)
	{
	case PYRINAS_CLOUD_EVT_READY:
		/* Start main thread */
		k_sem_give(&main_thread_proceed_sem);
		break;
	case PYRINAS_CLOUD_EVT_DISCONNECTED:
		LOG_WRN("Disconnected!");
		k_delayed_work_submit(&reconnect_work, K_SECONDS(2));
		break;
	default:
		break;
	}
}

void reconnect_work_fn(struct k_work *item)
{
	LOG_INF("Reconnect work");

	int err = pyrinas_cloud_connect();
	if (err && err != EINPROGRESS)
	{
		LOG_WRN("Unable to re-connect. Err: %d", err);
		k_delayed_work_submit(&reconnect_work, K_SECONDS(10));
	}
}

void handle_nrf_modem_lib_init_ret(void)
{
	int ret = nrf_modem_lib_get_init_ret();

	/* Handle return values relating to modem firmware update */
	switch (ret)
	{
	case MODEM_DFU_RESULT_OK:
		LOG_INF("MODEM UPDATE OK. Will run new firmware");
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case MODEM_DFU_RESULT_UUID_ERROR:
	case MODEM_DFU_RESULT_AUTH_ERROR:
		LOG_ERR("MODEM UPDATE ERROR %d. Will run old firmware", ret);
		sys_reboot(SYS_REBOOT_COLD);
		break;
	case MODEM_DFU_RESULT_HARDWARE_ERROR:
	case MODEM_DFU_RESULT_INTERNAL_ERROR:
		LOG_ERR("MODEM UPDATE FATAL ERROR %d. Modem failiure", ret);
		sys_reboot(SYS_REBOOT_COLD);
		break;
	default:
		break;
	}
}
#endif

void main(void)
{

#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)

	/* check FOTA result */
	handle_nrf_modem_lib_init_ret();

#endif

/* All initializations were successful mark image as working so that we
		 * will not revert upon reboot.
		 */
#ifdef CONFIG_IMG_MANAGER
	boot_write_img_confirmed();
#endif

	/* Init flash */
	flash_init();

/* Img management with SMP */
#ifdef CONFIG_MCUMGR_CMD_IMG_MGMT
	img_mgmt_register_group();
#endif

/* Setting up the RTC on I2C2 */
#ifdef CONFIG_PCF85063A
	rtc_init();
#endif

#if defined(CONFIG_PYRINAS_CENTRAL_ENABLED)
	ble_stack_init_t bt_init = {
		.mode = ble_mode_central,
	};

	/* BLE initialization */
	ble_stack_init(&bt_init);

#endif

#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)

#if defined(CONFIG_PYRINAS_CLOUD_PROVISION_CERTIFICATES)
	int err = nrf_modem_lib_init(NORMAL_MODE);
	if (err)
	{
		LOG_ERR("Failed to initialize modem library!");
	}

	/* Initialize AT comms in order to provision the certificate */
	err = at_cmd_init();
	if (err)
	{
		printk("Failed to initialize AT commands, err %d\n", err);
	}

	/* Provision OTA CA cert */
	err = pyrinas_cloud_ota_cert_provision();
	if (err)
	{
		LOG_ERR("Error provisioning OTA cert. Code: %d", err);
	}
#endif

	/* Configure modem */
	cellular_configure();

	/* Configure modem params */
	cellular_info_init();

	/* Work init*/
	k_delayed_work_init(&reconnect_work, reconnect_work_fn);

	/* Init Pyrinas Cloud */
	struct pyrinas_cloud_config config = {
		.evt_cb = pyrinas_cloud_evt_handler,
	};

	pyrinas_cloud_init(&config);

	/* Connect */
	err = pyrinas_cloud_connect();
	if (err)
	{
		LOG_ERR("Unable to connect to Pyrinas cloud. Code: %d", err);
	}

#else
	k_sem_give(&main_thread_proceed_sem);
#endif

	/* Wait for ready */
	k_sem_take(&main_thread_proceed_sem, K_FOREVER);

	/* User Setup function */
	setup();
}

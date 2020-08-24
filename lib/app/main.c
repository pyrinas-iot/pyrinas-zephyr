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
#include <modem/lte_lc.h>
#include <modem/bsdlib.h>
#include <modem/at_cmd.h>
#include <modem/at_notif.h>
#include <dfu/mcuboot.h>
#include <app/app.h>

#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)
#include <bsd.h>
#endif

#include <cellular/cellular.h>
#include <pyrinas_cloud/pyrinas_cloud.h>

#ifdef CONFIG_MCUMGR_CMD_IMG_MGMT
#include "img_mgmt/img_mgmt.h"
#endif

#include <logging/log.h>
LOG_MODULE_REGISTER(main);

#define GPIO0 DT_LABEL(DT_NODELABEL(gpio0))
struct device *gpio;

static K_SEM_DEFINE(main_thread_proceed, 0, 1);

#if defined(CONFIG_FILE_SYSTEM_LITTLEFS)
#include <fs/fs.h>
#include <fs/littlefs.h>
#include <storage/flash_map.h>

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
static struct fs_mount_t lfs_storage_mnt = {
		.type = FS_LITTLEFS,
		.fs_data = &storage,
		.storage_dev = (void *)FLASH_AREA_ID(storage),
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
		LOG_ERR("FAIL: unable to find flash area %u: %d\n",
						id, rc);
		return;
	}

	LOG_INF("Area %u at 0x%x on %s for %u bytes\n",
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
		LOG_ERR("FAIL: mount id %u at %s: %d\n",
						(unsigned int)mp->storage_dev, mp->mnt_point,
						rc);
		return;
	}
	LOG_INF("%s mount: %d\n", mp->mnt_point, rc);

	rc = fs_statvfs(mp->mnt_point, &sbuf);
	if (rc < 0)
	{
		LOG_ERR("FAIL: statvfs: %d\n", rc);
		return;
	}

	LOG_INF("%s: bsize = %lu ; frsize = %lu ;"
					" blocks = %lu ; bfree = %lu\n",
					mp->mnt_point,
					sbuf.f_bsize, sbuf.f_frsize,
					sbuf.f_blocks, sbuf.f_bfree);
#endif
}

/* RTC control */
struct device *rtc;

#ifdef CONFIG_PCF85063A
static void rtc_init()
{

	// Get the device
	rtc = device_get_binding("PCF85063A");
	if (rtc == NULL)
	{
		LOG_ERR("Failed to get RTC device binding\n");
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

#ifdef CONFIG_PCF85063A
static bool timer_flag = false;
#endif

void main(void)
{

#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)
	int err;

#if !defined(CONFIG_BSD_LIBRARY_SYS_INIT)
	err = bsdlib_init();
#else
	/* If bsdlib is initialized on post-kernel we should
		 * fetch the returned error code instead of bsdlib_init
		 */
	err = bsdlib_get_init_ret();
#endif
	switch (err)
	{
	case MODEM_DFU_RESULT_OK:
		printk("Modem firmware update successful!\n");
		printk("Modem will run the new firmware after reboot\n");
		k_thread_suspend(k_current_get());
		break;
	case MODEM_DFU_RESULT_UUID_ERROR:
	case MODEM_DFU_RESULT_AUTH_ERROR:
		printk("Modem firmware update failed\n");
		printk("Modem will run non-updated firmware on reboot.\n");
		break;
	case MODEM_DFU_RESULT_HARDWARE_ERROR:
	case MODEM_DFU_RESULT_INTERNAL_ERROR:
		printk("Modem firmware update failed\n");
		printk("Fatal error.\n");
		break;
	case -1:
		printk("Could not initialize bsdlib.\n");
		printk("Fatal error.\n");
		return;
	default:
		break;
	}
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

	/* Early setup before cloud functions */
	early_setup();

#if defined(CONFIG_PYRINAS_CLOUD_ENABLED)
	/* Configure modem */
	cellular_configure();

	/* Configure modem params */
	cellular_info_init();

	/* Init Pyrinas Cloud */
	pyrinas_cloud_init();

	/* Connect */
	pyrinas_cloud_connect();
#endif

	/* User Setup function */
	setup();

	/* Start main thread */
	k_sem_give(&main_thread_proceed);
}

void main_thread_fn()
{
	k_sem_take(&main_thread_proceed, K_FOREVER);

	while (true)
	{
#ifdef CONFIG_PCF85063A
		if (!timer_flag)
		{
			int ret = counter_get_pending_int(rtc);
			LOG_INF("Interrupt? %d", ret);

			if (ret == 1)
			{
				timer_flag = true;

				int ret = counter_cancel_channel_alarm(rtc, 0);
				if (ret)
				{
					LOG_ERR("Unable to cancel channel alarm!");
				}
			}
		}
#endif

		/* User loop function */
		loop();

#ifdef PYRINAS_ENABLED
		/* BLE Process */
		ble_process();
#endif
	}
}

#define THREAD_STACK_SIZE KB(4)
static K_THREAD_STACK_DEFINE(main_thread_stack, THREAD_STACK_SIZE);
K_THREAD_DEFINE(main_thread, K_THREAD_STACK_SIZEOF(main_thread_stack),
								main_thread_fn, NULL, NULL, NULL,
								K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

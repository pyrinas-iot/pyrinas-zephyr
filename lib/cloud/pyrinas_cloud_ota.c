#include <stdio.h>

#include <storage/flash_map.h>
#include <dfu/mcuboot.h>
#include <zephyr/dfu/flash_img.h>
#include <pyrinas_cloud/pyrinas_version.h>
#include <pyrinas_cloud/pyrinas_cloud_ota.h>
#include <pyrinas_cloud/pyrinas_cloud_codec.h>
#include <pyrinas_cloud/pyrinas_cloud_helper.h>

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_cloud_ota);

/* Cloud state callbacks */
static struct pyrinas_cloud_config m_config;

/* Topic */
char ota_pub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_OTA_TOPIC) + PYRINAS_DEV_ID_LEN + 1];

/* Making the ota dat static */
static struct pyrinas_cloud_ota_package ota_package;
static struct pyrinas_cloud_ota_download next_block = {0};
static struct pyrinas_cloud_ota_request req = {0};

/* OTA context */
struct flash_img_context flash_img_ctx;

static int pyrinas_cloud_ota_prepare(struct flash_img_context *ctx)
{
	int err;

	if (mcuboot_swap_type() == BOOT_SWAP_TYPE_REVERT)
	{
		LOG_ERR("Flash is not in valid state!");
		return -EBUSY;
	}

	err = flash_img_init(ctx);
	if (err)
	{
		LOG_ERR("failed to init: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_IMG_ERASE_PROGRESSIVELY))
	{
		return 0;
	}

	err = flash_area_erase(ctx->flash_area, 0, ctx->flash_area->fa_size);
	if (err)
	{
		return err;
	}

	return 0;
}

int pyrinas_cloud_ota_set_package(uint8_t *data, size_t len)
{

	int result = 0;

	/* Parse OTA event */
	int err = decode_ota_package(&ota_package, data, len);
	if (err)
	{
		LOG_WRN("ota: Unable to decode OTA data");
		return err;
	}
	else
	{
		/* Check numeric */
		result = ver_comp(&pyrinas_version, &ota_package.version);

		/* Print result */
		LOG_INF("ota: New version? %s ", result == 1 ? "true" : "false");
		LOG_INF("ota: Remote version: %i.%i.%i-%i ", ota_package.version.major, ota_package.version.minor, ota_package.version.patch, ota_package.version.commit);
		LOG_INF("ota: Identifier: %s", (char *)ota_package.id);
		LOG_INF("ota: Size: %d", ota_package.size);

		/* If incoming is greater or hash is not equal */
		if (result == 1)
		{

			LOG_INF("ota: Start upgrade");

			/* Send to calback */
			if (m_config.evt_cb)
			{
				const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_START};
				m_config.evt_cb(&evt);
			}
		}
	}

	return 0;
}

int pyrinas_cloud_ota_init(struct pyrinas_cloud_config *p_config)
{

	if (p_config == NULL)
	{
		return -EINVAL;
	}

	snprintf(ota_pub_topic, sizeof(ota_pub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_TOPIC, p_config->client_id.len, p_config->client_id.str, 'p');

	LOG_DBG("%s %i %i", (char *)ota_pub_topic, strlen(ota_pub_topic), sizeof(ota_pub_topic));

	m_config = *p_config;

	return 0;
}

int pyrinas_cloud_ota_start()
{
	/* Start the FOTA process */
	int err;
	char buf[sizeof(struct pyrinas_cloud_ota_request) + 10];
	size_t size = 0;

	LOG_DBG("%s", (char *)ota_package.id);

	/* Init flash slot */
	err = pyrinas_cloud_ota_prepare(&flash_img_ctx);
	if (err)
	{
		LOG_ERR("Failure preparing for OTA. Error: %d", err);

		/* Stop update process. */
		const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
						      .err = err};
		m_config.evt_cb(&evt);

		return err;
	}

	/* Encode the request */
	req.type = ota_cmd_type_download_bytes;
	req.start_pos = 0;
	req.end_pos = CONFIG_PYRINAS_CLOUD_MQTT_OTA_BLOCK_SIZE;

	/* Copy over filename */
	memcpy(req.id, ota_package.id, strlen(ota_package.id));

	LOG_DBG("Next: %i %i", req.start_pos, req.end_pos);

	/* Encode request */
	encode_ota_request(&req, buf, sizeof(buf), &size);

	/* Publish the data */
	err = pyrinas_cloud_publish_raw(ota_pub_topic, strlen(ota_pub_topic), buf, size);
	if (err)
	{
		LOG_ERR("Unable to publish OTA check. Error: %d", err);

		/* Stop update process. */
		const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
						      .err = -ENOTSUP};
		m_config.evt_cb(&evt);

		return err;
	}

	return 0;
}

int pyrinas_cloud_ota_set_next(uint8_t *data, size_t len)
{

	int err;

	if (ota_package.size == 0)
	{
		LOG_WRN("OTA current download not set!");
		return -EINVAL;
	}

	/* Decode */
	err = decode_ota_download(&next_block, data, len);
	if (err)
	{

		LOG_ERR("Unable to decode payload: %d", err);

		/* Send to calback */
		if (m_config.evt_cb)
		{
			const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
							      .err = err};
			m_config.evt_cb(&evt);
		}

		return err;
	}

	LOG_DBG("Payload %i %i", next_block.start_pos, next_block.end_pos);

	/* Ignore already recieved payloads */
	if (next_block.start_pos != req.start_pos ||
	    next_block.end_pos != req.end_pos)
	{
		LOG_ERR("%i != %i %i != %i", next_block.start_pos, req.start_pos,
			next_block.end_pos, req.end_pos);
		return -EINVAL;
	}

	/* Send to calback */
	if (m_config.evt_cb)
	{
		const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_NEXT,
						      .err = err};
		m_config.evt_cb(&evt);
	}

	return 0;
}

int pyrinas_cloud_ota_process_next()
{
	int err;
	char buf[sizeof(struct pyrinas_cloud_ota_request) + 10];
	size_t size = 0;
	bool last = false;

	/* Make sure we got the right block */
	if (next_block.start_pos != req.start_pos ||
	    next_block.end_pos != req.end_pos)
	{
		return -EINVAL;
	}

	/* Are we at the end? */
	last = next_block.end_pos == ota_package.size;

	/* Write to flash */
	err = flash_img_buffered_write(&flash_img_ctx, next_block.data, next_block.len, last);
	if (err)
	{
		LOG_ERR("Failed to write to flash: %d", err);

		/* Send to calback */
		if (m_config.evt_cb)
		{
			const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
							      .err = err};
			m_config.evt_cb(&evt);
		}

		return err;
	}

	/* Finish up! */
	if (last)
	{
		/* Upgrade! */
		err = boot_request_upgrade(BOOT_UPGRADE_TEST);
		if (err)
		{
			LOG_ERR("Failed to request upgrade: %i", err);

			/* Send to calback */
			if (m_config.evt_cb)
			{
				const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
								      .err = err};
				m_config.evt_cb(&evt);
			}
		}
		else
		{
			/* Send to calback */
			if (m_config.evt_cb)
			{
				const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_DONE};
				m_config.evt_cb(&evt);
			}
		}

		return 0;
	}

	/* Otherwise, process and select next block */
	uint32_t end_pos = next_block.end_pos + CONFIG_PYRINAS_CLOUD_MQTT_OTA_BLOCK_SIZE;
	if (end_pos > ota_package.size)
	{
		end_pos = ota_package.size;
	}

	/* NOTE: end position is not included in the range. So if 0 - 1024, we only get 0-1023 */

	/* Encode the request */
	req.type = ota_cmd_type_download_bytes;
	req.start_pos = next_block.end_pos;
	req.end_pos = end_pos;

	/* Copy over filename */
	memcpy(req.id, ota_package.id, strlen(ota_package.id));

	LOG_DBG("Next: %i %i", req.start_pos, req.end_pos);

	/* Encode */
	encode_ota_request(&req, buf, sizeof(buf), &size);

	/* Publish the data */
	err = pyrinas_cloud_publish_raw(ota_pub_topic, strlen(ota_pub_topic), buf, size);
	if (err)
	{
		LOG_ERR("Unable to publish OTA request. Error: %d", err);

		/* Stop update process. */
		const struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
						      .err = err};
		m_config.evt_cb(&evt);

		return err;
	}

	return 0;
}

int pyrinas_cloud_ota_check()
{
	char buf[10];
	size_t size = 0;

	struct pyrinas_cloud_ota_request req = {
	    .type = ota_cmd_type_check};

	/* Encode the request */
	encode_ota_request(&req, buf, sizeof(buf), &size);

	/* Publish the data */
	int err = pyrinas_cloud_publish_raw(ota_pub_topic, strlen(ota_pub_topic), buf, size);
	if (err)
	{
		LOG_ERR("Unable to publish OTA check. Error: %d", err);
		return err;
	}

	return 0;
}

static int ota_done()
{
	char buf[10];
	size_t size = 0;

	struct pyrinas_cloud_ota_request req = {
	    .type = ota_cmd_type_done};

	/* Encode the request */
	encode_ota_request(&req, buf, sizeof(buf), &size);

	/* Publish the data */
	int err = pyrinas_cloud_publish_raw(ota_pub_topic, strlen(ota_pub_topic), buf, size);
	if (err)
	{
		LOG_ERR("Unable to publish OTA done. Error: %d", err);
		return err;
	}

	return 0;
}

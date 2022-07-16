#ifndef _PYRINAS_CLOUD_OTA_H
#define _PYRINAS_CLOUD_OTA_H

enum pyrinas_cloud_ota_image_type
{
	pyrinas_cloud_ota_image_type_primary = (1 << 0),
	pyrinas_cloud_ota_image_type_secondary = (1 << 1)
};

struct pyrinas_cloud_ota_download
{
	uint32_t start_pos;
	uint32_t end_pos;
	uint8_t data[CONFIG_PYRINAS_CLOUD_MQTT_OTA_BLOCK_SIZE];
	size_t len;
};

struct pyrinas_cloud_ota_package
{
	char id[PYRINAS_OTA_PACKAGE_MAX_FILE_PATH_CHARS];
	union pyrinas_cloud_ota_version version;
	/* File data placeholder */
	size_t size;
	/* Date added placeholder */
};

int pyrinas_cloud_ota_init(struct pyrinas_cloud_config *p_config);
int pyrinas_cloud_ota_start();
int pyrinas_cloud_ota_set_package(uint8_t *data, size_t len);
int pyrinas_cloud_ota_set_next(uint8_t *data, size_t len);
int pyrinas_cloud_ota_process_next();
int pyrinas_cloud_ota_check();

#endif /* _PYRINAS_CLOUD_OTA_H */

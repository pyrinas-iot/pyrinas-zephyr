
/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _PYRINAS_CLOUD_H
#define _PYRINAS_CLOUD_H

#include <zephyr.h>
#include <pyrinas_codec.h>

/* Defines */
#define IMEI_LEN 15
#define PYRINAS_OTA_PACKAGE_MAX_FILE_COUNT 2
#define PYRINAS_OTA_PACKAGE_MAX_FILE_PATH_CHARS 128

/* Settings keys */
#define PYRINAS_CLOUD_USER "pyrinas/username"
#define PYRINAS_CLOUD_PASSWORD "pyrinas/password"
#define PYRINAS_CLOUD_HOSTNAME "pyrinas/hostname"
#define PYRINAS_CLOUD_PORT "pyrinas/port"

/* Used to encode telemetry related keys */
enum pyrinas_cloud_telemetry_type
{
  tel_type_version,
  tel_type_rsrp,
  tel_type_rssi_central,    /* Bluetooth RSSI at central */
  tel_type_rssi_peripheral, /* Bluetooth RSSI at client */
};

/* Used to encode and decode ota related keys */
enum pyrinas_cloud_ota_type
{
  ota_type_ver,
  ota_type_url,
  ota_type_force,
};

/* Used to encode and decode ota related keys */
enum pyrinas_cloud_ota_cmd_type
{
  ota_cmd_type_check,
  ota_cmd_type_done,
};

/* Used to determine which OTA verison to use */
enum pyrinas_cloud_ota_request_version
{
  ota_request_version_unknown = 0,
  ota_request_version_v1,
  ota_request_version_v2,
};

/* Event type */
enum pyrinas_cloud_evt_type
{
  PYRINAS_CLOUD_EVT_CONNECTING,
  PYRINAS_CLOUD_EVT_CONNECTED,
  PYRINAS_CLOUD_EVT_READY,
  PYRINAS_CLOUD_EVT_ERROR,
  PYRINAS_CLOUD_EVT_DISCONNECTED,
  PYRINAS_CLOUD_EVT_DATA_RECIEVED,
  PYRINAS_CLOUD_EVT_FOTA_START,
  PYRINAS_CLOUD_EVT_FOTA_DOWNLOADING,
  PYRINAS_CLOUD_EVT_FOTA_DONE,
  PYRINAS_CLOUD_EVT_FOTA_REBOOTING,
  PYRINAS_CLOUD_EVT_FOTA_ERROR,
};

struct pyrinas_cloud_evt_data
{
  uint8_t topic[CONFIG_PYRINAS_CLOUD_MQTT_TOPIC_SIZE];
  size_t topic_len;
  uint8_t data[CONFIG_PYRINAS_CLOUD_MQTT_PAYLOAD_SIZE];
  size_t data_len;
};

/* Used for different states of Pyrinas Cloud*/
struct pyrinas_cloud_evt
{
  enum pyrinas_cloud_evt_type type;
  union
  {
    int err;
    struct pyrinas_cloud_evt_data msg;
  } data;
};

/* Used to track the state of OTA internally to pyrinas_cloud*/
enum pyrinas_cloud_ota_state
{
  ota_state_ready,
  ota_state_started,
  ota_state_downloading,
  ota_state_done,
  ota_state_error,
  ota_state_reboot,
  ota_state_rebooting
};

/* Used to track the state of the connection internally to pyrinas_cloud*/
enum pryinas_cloud_state
{
  cloud_state_disconnected,
  cloud_state_connected,
};

struct pyrinas_cloud_telemetry_data
{
  bool has_version;
  char version[24];
  bool has_rsrp;
  char rsrp;
  bool has_central_rssi;
  int8_t central_rssi;
  bool has_peripheral_rssi;
  int8_t peripheral_rssi;
};

union pyrinas_cloud_ota_version
{
  struct
  {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    uint8_t commit;
    uint8_t hash[8];
  };
  uint8_t raw[12];
};

struct pyrinas_cloud_settings_params
{
  bool found;
  char *buf;
  size_t len;
};

enum pyrinas_cloud_ota_image_type
{
  pyrinas_cloud_ota_image_type_primary = (1 << 0),
  pyrinas_cloud_ota_image_type_secondary = (1 << 1)
};

struct pyrinas_cloud_ota_file_info
{
  enum pyrinas_cloud_ota_image_type image_type;
  char host[PYRINAS_OTA_PACKAGE_MAX_FILE_PATH_CHARS];
  char file[PYRINAS_OTA_PACKAGE_MAX_FILE_PATH_CHARS];
};

struct pyrinas_cloud_ota_package
{
  union pyrinas_cloud_ota_version version;
  struct pyrinas_cloud_ota_file_info files[PYRINAS_OTA_PACKAGE_MAX_FILE_COUNT];
};

/* Callbacks */
typedef void (*pyrinas_cloud_evt_cb_t)(const struct pyrinas_cloud_evt *const p_evt);
typedef void (*pyrinas_cloud_application_cb_t)(const uint8_t *topic, size_t topic_len, const uint8_t *data, size_t data_len);

typedef struct
{
  pyrinas_cloud_application_cb_t cb;
  char full_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC) + IMEI_LEN + CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE];
  char topic[CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE];
  size_t topic_len;
} pryinas_cloud_application_cb_entry_t;

/* Saving variable length client ID*/
struct pryinas_cloud_client_id
{
  char *str;
  size_t len;
};

/* Pyrinas Cloud Config */
struct pyrinas_cloud_config
{
  pyrinas_cloud_evt_cb_t evt_cb;
  struct pryinas_cloud_client_id client_id;
};

/* Client init info */
struct pyrinas_cloud_client_init
{
  char *username;
  char *password;
  uint16_t port;
  char *hostname;
};

/* Init MQTT Client */
int pyrinas_cloud_init(struct pyrinas_cloud_config *p_config);

/* Connects to Pyrinas*/
int pyrinas_cloud_connect();

/* Disconnects from Pyrinas */
int pyrinas_cloud_disconnect();

/* Determines if conected */
bool pyrinas_cloud_is_connected();

/* Register client device */
int pyrinas_cloud_register_uid(char *uid);

/* Unregister client device */
int pyrinas_cloud_unregister_uid(char *uid);

/* Subscribe and listen for central specific application events */
int pyrinas_cloud_subscribe(char *name, pyrinas_cloud_application_cb_t callback);

/* Unsubscribe from event */
int pyrinas_cloud_unsubscribe(char *name);

/* Publish typed Pyrinas event */
int pyrinas_cloud_publish_evt(pyrinas_event_t *evt);

/* Publish central event to the cloud */
int pyrinas_cloud_publish(char *type, uint8_t *data, size_t len);

/* Publish telemetry only */
int pyrinas_cloud_publish_evt_telemetry(pyrinas_event_t *evt);

/* Publish telemetry data */
int pyrinas_cloud_publish_telemetry(struct pyrinas_cloud_telemetry_data *data);

/* Used during polling process */
void pyrinas_cloud_process();

#endif /* _PYRINAS_CLOUD_H */
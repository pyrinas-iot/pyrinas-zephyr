
#ifndef _PYRINAS_CLOUD_H
#define _PYRINAS_CLOUD_H

/* Defines */
#define IMEI_LEN 15

/* Used to encode telemetry related keys */
enum pyrinas_cloud_telemetry_type
{
  tel_type_version,
  tel_type_rsrp,
  tel_type_rssi_hub,    /* Bluetooth RSSI at hub */
  tel_type_rssi_client, /* Bluetooth RSSI at client */
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

enum pryinas_cloud_state
{
  cloud_state_disconnected,
  cloud_state_connected,
  cloud_state_force_disconnected,
};

struct pyrinas_cloud_ota_data
{
  char version[10];
  char host[128];
  char file[128];
  bool force;
};

/* Callbacks */
typedef void (*pyrinas_cloud_state_evt_t)(enum pryinas_cloud_state evt);
typedef void (*pyrinas_cloud_application_cb_t)(const uint8_t *topic, size_t topic_len, const uint8_t *data, size_t data_len);

typedef struct
{
  pyrinas_cloud_application_cb_t cb;
  char full_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC) + IMEI_LEN + CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE];
  char topic[CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE];
  size_t topic_len;
} pryinas_cloud_application_cb_entry_t;

/* Init MQTT Client */
void pyrinas_cloud_init();

void pyrinas_cloud_test();

int pyrinas_cloud_connect();
int pyrinas_cloud_disconnect();

void pyrinas_cloud_start();

void pyrinas_cloud_process();

bool pyrinas_cloud_is_connected();

void pyrinas_cloud_register_state_evt(pyrinas_cloud_state_evt_t cb);

/* Register client device */
int pyrinas_cloud_register_uid(char *uid);

/* Unregister client device */
int pyrinas_cloud_unregister_uid(char *uid);

/* Subscribe and listen for hub specific application events */
int pyrinas_cloud_subscribe(char *name, pyrinas_cloud_application_cb_t callback);

/* Unsubscribe from event */
int pyrinas_cloud_unsubscribe(char *name);

/* Publish hub event to the cloud */
int pyrinas_cloud_publish(char *type, uint8_t *data, size_t len);

/* Publish UID specific application event to the cloud */
int pyrinas_cloud_publish_w_uid(char *uid, char *type, uint8_t *data, size_t len);

/* Subscribe and listen for uid specific application events */
int pyrinas_cloud_subscribe_w_uid();

#endif /* _PYRINAS_CLOUD_H */
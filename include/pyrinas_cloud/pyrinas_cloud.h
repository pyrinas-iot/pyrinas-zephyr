
#ifndef _PYRINAS_CLOUD_H
#define _PYRINAS_CLOUD_H

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

/* Init MQTT Client */
void pyrinas_cloud_init();

void pyrinas_cloud_test();

int pyrinas_cloud_connect();
int pyrinas_cloud_disconnect();

void pyrinas_cloud_start();

void pyrinas_cloud_process();

bool pyrinas_cloud_is_connected();

/* Register client device */
int pyrinas_cloud_register_uid(char *uid);

/* Unregister client device */
int pyrinas_cloud_unregister_uid(char *uid);

/* Publish hub application event to the cloud */
int pyrinas_cloud_publish();

/* Subscribe and listen for hub specific application events */
int pyrinas_cloud_subscribe();

/* Publish UID specific application event to the cloud */
int pyrinas_cloud_publish_w_uid();

/* Subscribe and listen for uid specific application events */
int pyrinas_cloud_subscribe_w_uid();

#endif /* _PYRINAS_CLOUD_H */
#ifndef COMMS_H
#define COMMS_H

typedef enum
{
  comms_evt_read,
} comms_evt_type_t;

typedef struct
{
  comms_evt_type_t type;
  uint8_t *data;
  size_t len;
} comms_evt_t;

typedef void (*comms_evt_callback_t)(const comms_evt_t *evt);

int comms_write(const uint8_t *data, size_t data_len);
int comms_init(comms_evt_callback_t callback);

#endif
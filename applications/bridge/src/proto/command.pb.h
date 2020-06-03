/* Automatically generated nanopb header */
/* Generated by nanopb-0.3.9 at Wed Mar  4 20:16:43 2020. */

#ifndef PB_COMMAND_PB_H_INCLUDED
#define PB_COMMAND_PB_H_INCLUDED
#include <pb.h>

/* @@protoc_insertion_point(includes) */
#if PB_PROTO_HEADER_VERSION != 30
#error Regenerate this file with the current version of nanopb generator.
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Enum definitions */
typedef enum _event_type {
    event_type_command = 0,
    event_type_response = 1
} event_type;
#define _event_type_MIN event_type_command
#define _event_type_MAX event_type_response
#define _event_type_ARRAYSIZE ((event_type)(event_type_response+1))

/* Struct definitions */
typedef struct _rssi {
    int16_t rx;
    int16_t tx;
/* @@protoc_insertion_point(struct:rssi) */
} rssi;

typedef PB_BYTES_ARRAY_T(18) protobuf_event_t_name_t;
typedef PB_BYTES_ARRAY_T(128) protobuf_event_t_data_t;
typedef struct _protobuf_event_t {
    event_type type;
    protobuf_event_t_name_t name;
    protobuf_event_t_data_t data;
    pb_byte_t faddr[6];
    pb_byte_t taddr[6];
    rssi rssi;
/* @@protoc_insertion_point(struct:protobuf_event_t) */
} protobuf_event_t;

/* Default values for struct fields */

/* Initializer values for message structs */
#define rssi_init_default                        {0, 0}
#define protobuf_event_t_init_default            {(event_type)0, {0, {0}}, {0, {0}}, {0}, {0}, rssi_init_default}
#define rssi_init_zero                           {0, 0}
#define protobuf_event_t_init_zero               {(event_type)0, {0, {0}}, {0, {0}}, {0}, {0}, rssi_init_zero}

/* Field tags (for use in manual encoding/decoding) */
#define rssi_rx_tag                              1
#define rssi_tx_tag                              2
#define protobuf_event_t_type_tag                1
#define protobuf_event_t_name_tag                2
#define protobuf_event_t_data_tag                3
#define protobuf_event_t_faddr_tag               4
#define protobuf_event_t_taddr_tag               5
#define protobuf_event_t_rssi_tag                6

/* Struct field encoding specification for nanopb */
extern const pb_field_t rssi_fields[3];
extern const pb_field_t protobuf_event_t_fields[7];

/* Maximum encoded size of messages (where known) */
#define rssi_size                                22
#define protobuf_event_t_size                    193

/* Message IDs (where set with "msgid" option) */
#ifdef PB_MSGID

#define COMMAND_MESSAGES \


#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
/* @@protoc_insertion_point(eof) */

#endif

#include <zephyr.h>
#include <stdio.h>
#include <tinycbor/cbor.h>
#include <tinycbor/cbor_buf_writer.h>
#include <tinycbor/cbor_buf_reader.h>
#include <pyrinas_cloud/pyrinas_cloud.h>
#include <cellular/cellular.h>

#include "pyrinas_cloud_codec.h"

int encode_ota_request(enum pyrinas_cloud_ota_cmd_type cmd_type, uint8_t *buf, size_t data_len, size_t *payload_len)
{

    CborEncoder cbor, cbor_map;
    CborError cbor_err;
    struct cbor_buf_writer writer;
    const int keypair_count = 1;

    cbor_buf_writer_init(&writer, buf, data_len);
    cbor_encoder_init(&cbor, &writer.enc, 0);
    cbor_encoder_create_map(&cbor, &cbor_map, keypair_count);

    /* The ota command position */
    cbor_encode_uint(&cbor_map, 0);
    cbor_encode_uint(&cbor_map, cmd_type);

    cbor_err = cbor_encoder_close_container(&cbor, &cbor_map);
    if (cbor_err != 0)
    {
        printk("[%s:%d] cbor encoding error %d\n", __func__,
            __LINE__, cbor_err);
        return ENOEXEC;
    }

    /* Gets the length of the CBOR data*/
    *payload_len = (size_t)(writer.ptr - buf);

    printk("[%s:%d] cbor encoded %d bytes\n", __func__,
        __LINE__, *payload_len);

    return 0;

}

int decode_ota_data(struct pyrinas_cloud_ota_data *ota_data, const char *data, size_t data_len)
{

    CborError cbor_error;
    CborParser parser;
    CborValue value;
    struct cbor_buf_reader reader;

    /* initalize the reader */
    cbor_buf_reader_init(&reader, data, data_len);
    cbor_error = cbor_parser_init(&reader.r, 0, &parser, &value);

    if (cbor_error != CborNoError)
    {
        printk("CBOR parser initialization failed (err: %d)\n",
            cbor_error);
        return cbor_error;
    }

    /* Return if we're not dealing with a map*/
    if (!cbor_value_is_map(&value))
    {
        printk("Unexpected CBOR data structure.\n");
        return -ENOEXEC;
    }

    // Since int is a known size we can use the fixed funciton
    CborValue map_value;
    // int type;
    cbor_value_enter_container(&value, &map_value);

    /* Get the version */
    cbor_value_advance_fixed(&map_value);
    size_t ver_str_len = sizeof(ota_data->version);
    cbor_value_copy_text_string(&map_value, ota_data->version,
        &ver_str_len, &map_value); /* this advances the iterator*/

/* Get the host */
    cbor_value_advance_fixed(&map_value);
    size_t host_str_len = sizeof(ota_data->host);
    cbor_value_copy_text_string(&map_value, ota_data->host,
        &host_str_len, &map_value); /* this advances the iterator*/

/* Get the file */
    cbor_value_advance_fixed(&map_value);
    size_t file_str_len = sizeof(ota_data->file);
    cbor_value_copy_text_string(&map_value, ota_data->file,
        &file_str_len, &map_value); /* this advances the iterator*/

/* Get the force value */
    cbor_value_advance_fixed(&map_value);
    cbor_value_get_boolean(&map_value, &ota_data->force);

    printk("version:%s, url:%s/%s, force:%d\n", ota_data->version, ota_data->host, ota_data->file, ota_data->force);

    return 0;
}

int encode_telemetry_data(uint8_t *buf, size_t data_len, size_t *payload_len)
{
    CborEncoder cbor, cbor_map;
    CborError cbor_err;
    struct cbor_buf_writer writer;

    /* This is dynamic */
    uint8_t keypair_count = 2;

    /* Sends the current signal strength */
    char rsrp = cellular_get_signal_strength();

    /* Decrement keypair count if rsrp (signal strength) is invalid */
    if (rsrp > RSRP_THRESHOLD)
    {
        keypair_count -= 1;
    }

    cbor_buf_writer_init(&writer, buf, data_len);
    cbor_encoder_init(&cbor, &writer.enc, 0);
    cbor_encoder_create_map(&cbor, &cbor_map, keypair_count);

    /* Sends the app version string as provided by Git*/
    // char type[] = {tel_type_version};
    // cbor_encode_text_stringz(&cbor_map, "version");
    cbor_encode_uint(&cbor_map, tel_type_version);
    cbor_encode_text_stringz(&cbor_map, STRINGIFY(PYRINAS_APP_VERSION));

    /* Only add if valid */
    if (rsrp <= RSRP_THRESHOLD)
    {
        // type[0] = tel_type_rsrp;
        // cbor_encode_text_stringz(&cbor_map, "rsrp");
        cbor_encode_uint(&cbor_map, tel_type_rsrp);
        cbor_encode_uint(&cbor_map, rsrp);
    }

    cbor_err = cbor_encoder_close_container(&cbor, &cbor_map);
    if (cbor_err != 0)
    {
        printk("[%s:%d] cbor encoding error %d\n", __func__,
            __LINE__, cbor_err);
        return ENOEXEC;
    }

    /* Gets the length of the CBOR data*/
    *payload_len = (size_t)(writer.ptr - buf);

    printk("[%s:%d] cbor encoded %d bytes\n", __func__,
        __LINE__, *payload_len);

    return 0;
}
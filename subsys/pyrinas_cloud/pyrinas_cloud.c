#include <zephyr.h>
#include <stdio.h>
#include <drivers/uart.h>
#include <string.h>
#include <modem/at_cmd.h>
#include <random/rand32.h>
#include <net/mqtt.h>
#include <net/socket.h>
#include <modem/lte_lc.h>
#include <pyrinas_cloud/pyrinas_cloud.h>
#include <cellular/cellular.h>
#include <net/fota_download.h>
#include <power/reboot.h>
#include <assert.h>

#include "pyrinas_cloud_codec.h"
#include "pyrinas_cloud_helper.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_cloud);

#if !defined(CONFIG_MQTT_LIB_TLS)
#error CONFIG_MQTT_LIB_TLS must be defined!
#endif /* defined(CONFIG_MQTT_LIB_TLS) */

static void telemetry_check_event(struct k_timer *timer);
K_TIMER_DEFINE(telemetry_timer, telemetry_check_event, NULL);

static void reconnect_timer_event(struct k_timer *timer);
K_TIMER_DEFINE(reconnect_timer, reconnect_timer_event, NULL);

/* Telemetry interval */
#define TELEMETRY_INTERVAL K_MINUTES(10)

/* Reconnect interval */
#define RECONNECT_INTERVAL K_SECONDS(30)

/* Security tag for fetching certs */
static sec_tag_t sec_tag_list[] = {CONFIG_PYRINAS_CLOUD_SEC_TAG};

/* Buffers for MQTT client. */
static uint8_t rx_buffer[CONFIG_PYRINAS_CLOUD_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_PYRINAS_CLOUD_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t payload_buf[CONFIG_PYRINAS_CLOUD_MQTT_PAYLOAD_BUFFER_SIZE];

/* IMEI storage */
#define CGSN_RESP_LEN 19
char imei[IMEI_LEN];

/* Topics */
char ota_pub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_OTA_PUB_TOPIC) + IMEI_LEN];
char ota_sub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_OTA_SUB_TOPIC) + IMEI_LEN];
char telemetry_pub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_PUB_TOPIC) + IMEI_LEN];
char application_sub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC) + IMEI_LEN + CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE];
// TODO: config topic -- used for adding/removing bluetooth clients

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Thread control */
static K_SEM_DEFINE(pyrinas_cloud_thread_sem, 0, 1);

/* Atomic flags */
static atomic_val_t cloud_state_s = ATOMIC_INIT(cloud_state_disconnected);
static atomic_val_t ota_state_s = ATOMIC_INIT(ota_state_ready);
static atomic_val_t startup_complete_s = ATOMIC_INIT(0);
static atomic_val_t reconnect = ATOMIC_INIT(0);
static atomic_val_t initial_ota_check = ATOMIC_INIT(0);

/* File descriptor */
static struct pollfd fds;

/* Structures for work */
static struct k_work state_update_work;
static struct k_work publish_telemetry_work;
static struct k_work ota_request_work;
static struct k_work ota_reboot_work;
static struct k_work ota_done_work;
static struct k_work on_connect_work;
static struct k_work reconnect_work;
static struct k_delayed_work fota_work;
static struct k_delayed_work disconnect_work;

/* Thread id */
static k_tid_t pyrinas_cloud_thread_id;

/* Making the ota dat static */
static struct pyrinas_cloud_ota_data ota_data;

/* Callback for application back to main context*/
pryinas_cloud_application_cb_entry_t *callbacks[CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT];

/* Cloud state callbacks */
static pyrinas_cloud_state_evt_t evt_callback = NULL;
static pyrinas_cloud_state_evt_t state_event = NULL;

/* Statically track message id*/
static uint16_t ota_sub_message_id = 0;

/* Version string */
static const char *version_string = STRINGIFY(PYRINAS_APP_VERSION);

#if defined(CONFIG_BSD_LIBRARY)

/**@brief Recoverable BSD library error. */
void bsd_recoverable_error_handler(uint32_t err)
{
    printk("bsdlib recoverable error: %u\n", (unsigned int)err);
}

#endif /* defined(CONFIG_BSD_LIBRARY) */

int pyrinas_cloud_subscribe(char *topic, pyrinas_cloud_application_cb_t callback)
{
    /* Get the string length */
    size_t topic_len = strlen(topic);

    // Return if name is greater than name size
    if (topic_len > CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE)
    {
        return -EINVAL;
    }

    for (int i = 0; i < CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT; i++)
    {
        if (callbacks[i] == NULL)
        {
            /* Allocate memory */
            callbacks[i] = k_malloc(sizeof(pryinas_cloud_application_cb_entry_t));

            /* Set the full name */
            snprintf(callbacks[i]->full_topic, sizeof(application_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC, IMEI_LEN, imei, topic_len, topic);

            LOG_DBG("application subscribe to: %s", callbacks[i]->full_topic);

            /* Set the values */
            callbacks[i]->cb = callback;
            callbacks[i]->topic_len = topic_len;

            /* Copy the name*/
            memcpy(callbacks[i]->topic, topic, topic_len);

            return 0;
        }
    }

    /* Can't add new entry. They're all occupied! */
    return -ENOMEM;
}

int pyrinas_cloud_unsubscribe(char *topic)
{

    for (int i = 0; i < CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT; i++)
    {
        if (callbacks[i] == NULL)
            continue;

        if (strcmp(callbacks[i]->topic, topic) == 0)
        {
            /* Free memory */
            k_free(callbacks[i]);
            return 0;
        }
    }

    /* Entry not found */
    return -ENOENT;
}

static void reconnect_timer_event(struct k_timer *timer)
{
    LOG_DBG("Reconnect!\n");

    /* Start reconnect work */
    k_work_submit(&reconnect_work);

    /* Restart timer until it's stopped by a connected event */
    k_timer_start(&reconnect_timer, RECONNECT_INTERVAL, K_SECONDS(10));
}

/**@brief Function to get IMEI
 */
static int get_imei(char *imei_buf, size_t len)
{
    enum at_cmd_state at_state;

    char buf[CGSN_RESP_LEN];

    /* Fetch the IMEI using at cmd */
    int err = at_cmd_write("AT+CGSN", buf, CGSN_RESP_LEN,
                           &at_state);
    if (err)
    {
        printk("Error when trying to do at_cmd_write: %d, at_state: %d",
               err, at_state);
        return err;
    }

    /* Copy data to imei buf */
    memcpy(imei_buf, buf, len);

    return 0;
}

/**@brief Function to publish data on the configured topic
 */
static int data_publish(uint8_t *topic, size_t topic_len, uint8_t *data, size_t data_len, uint16_t message_id)
{

    if (atomic_get(&cloud_state_s) != cloud_state_connected)
    {
        LOG_WRN("Not connected. Unable to publish!");
        return -ENETDOWN;
    }

    struct mqtt_publish_param param;

    param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
    param.message.topic.topic.utf8 = topic;
    param.message.topic.topic.size = topic_len;
    param.message.payload.data = data;
    param.message.payload.len = data_len;
    param.message_id = message_id;
    param.dup_flag = 0;
    param.retain_flag = 0;

    // data_print("Publishing: ", data, data_len);
    LOG_INF("Publishing %d bytes to topic: %s len: %u", data_len, topic, topic_len);

    return mqtt_publish(&client, &param);
}

/**@brief Function to unsubscribe to the configured topic
 */
static int unsubscribe(char *topic, size_t len)
{
    struct mqtt_topic subscribe_topic = {
        .topic = {.utf8 = topic,
                  .size = len},
        .qos = MQTT_QOS_1_AT_LEAST_ONCE};

    const struct mqtt_subscription_list subscription_list = {
        .list = &subscribe_topic, .list_count = 1, .message_id = sys_rand32_get()};

    printk("Unsubscribing to: %s len %u\n", topic, len);

    return mqtt_unsubscribe(&client, &subscription_list);
}

/**@brief Function to subscribe to the configured topic
 */
static int subscribe(char *topic, size_t len, uint16_t message_id)
{
    struct mqtt_topic subscribe_topic = {
        .topic = {.utf8 = topic,
                  .size = len},
        .qos = MQTT_QOS_1_AT_LEAST_ONCE};

    const struct mqtt_subscription_list subscription_list = {
        .list = &subscribe_topic, .list_count = 1, .message_id = message_id};

    LOG_INF("Subscribing to: %s len %u", topic, len);

    return mqtt_subscribe(&client, &subscription_list);
}

/**@brief Function to read the published payload.
 */
static int publish_get_payload(struct mqtt_client *c,
                               uint8_t *write_buf,
                               size_t length)
{
    uint8_t *buf = write_buf;
    uint8_t *end = buf + length;

    if (length > sizeof(payload_buf))
    {
        return -EMSGSIZE;
    }
    while (buf < end)
    {
        int ret = mqtt_read_publish_payload_blocking(c, buf, end - buf);

        if (ret < 0)
        {
            return ret;
        }
        else if (ret == 0)
        {
            return -EIO;
        }
        buf += ret;
    }
    return 0;
}

static void publish_ota_check()
{

    char buf[10];
    size_t size = 0;

    /* Encode the request */
    encode_ota_request(ota_cmd_type_check, buf, sizeof(buf), &size);

    /* Publish the data */
    int err = data_publish(ota_pub_topic, strlen(ota_pub_topic), buf, size, sys_rand32_get());
    if (err)
    {
        LOG_ERR("Unable to publish OTA check. Error: %d", err);
    }
}

static void publish_ota_done()
{

    char buf[10];
    size_t size = 0;

    /* Encode the request */
    encode_ota_request(ota_cmd_type_done, buf, sizeof(buf), &size);

    /* Publish the data */
    int err = data_publish(ota_pub_topic, strlen(ota_pub_topic), buf, size, sys_rand32_get());
    if (err)
    {
        LOG_ERR("Unable to publish OTA done. Error: %d", err);
    }
}

static void publish_telemetry()
{

    char buf[64];
    size_t payload_len = 0;
    int err = 0;

    // Intialize data
    struct pyrinas_cloud_telemetry_data data = {
        .has_rsrp = true,
        .rsrp = cellular_get_signal_strength(),
    };

    LOG_DBG("RSRP %d %i", data.has_rsrp, data.rsrp);

    // Copy over version string
    strncpy(data.version, version_string, sizeof(data.version));

    // Encode data
    err = encode_telemetry_data(&data, buf, sizeof(buf), &payload_len);
    if (err)
    {
        LOG_ERR("Unable to encode telemetry data.");
        return;
    }

    // Publish telemetry
    err = data_publish(telemetry_pub_topic, strlen(telemetry_pub_topic), buf, payload_len, sys_rand32_get());
    if (err)
    {
        LOG_ERR("Unable to publish telemetry. Error: %d", err);
    }
}

static void publish_telemetry_work_fn(struct k_work *unused)
{
    /* Publish telemetry */
    publish_telemetry();
}

static void telemetry_check_event(struct k_timer *timer)
{

    /* Publish from work queue or else hard fault */
    k_work_submit(&publish_telemetry_work);

    /* Restart timer */
    k_timer_start(&telemetry_timer, TELEMETRY_INTERVAL, K_NO_WAIT);
}

static void on_connect_fn(struct k_work *unused)
{
    LOG_DBG("[%s:%d] on connect work function!", __func__, __LINE__);

    /* Publish telemetry */
    publish_telemetry();

    /* Trigger OTA check */
    if (atomic_get(&ota_state_s) == ota_state_ready)
    {

        /* Save this for later */
        ota_sub_message_id = (uint16_t)sys_rand32_get();

        /* Subscribe to OTA topic */
        subscribe(ota_sub_topic, strlen(ota_sub_topic), ota_sub_message_id);
    }
}

static void disconnect_work_fn(struct k_work *unused)
{
    pyrinas_cloud_disconnect();
}

static void fota_start_fn(struct k_work *unused)
{
    ARG_UNUSED(unused);

#ifdef CONFIG_FOTA_DOWNLOAD

    /* Start the FOTA process */
    int err;
    int sec_tag = -1;

    /* Set the security tag if TLS is enabled. */
#if defined(CONFIG_DOWNLOAD_CLIENT_TLS)
    sec_tag = CONFIG_PYRINAS_CLOUD_HTTPS_SEC_TAG;
#endif

    LOG_DBG("%s/%s using tag %d\n", ota_data.host, ota_data.file, sec_tag);

    /* Start download uses default port and APN*/
    err = fota_download_start(ota_data.host, ota_data.file, sec_tag, 0, NULL);
    if (err)
    {
        LOG_ERR("fota_download_start error %d\n", err);
        atomic_set(&ota_state_s, ota_state_error);

        /* Reboot on error.. */
        sys_reboot(0);
        return;
    }

    /* Set that we're busy now */
    atomic_set(&ota_state_s, ota_state_downloading);
#endif
}

static void publish_evt_handler(const char *topic, size_t topic_len, const char *data, size_t data_len)
{
    LOG_INF("topic: %s topic_len: %d data_len: %d", topic, topic_len, data_len);

    /* If its the OTA sub topic process */
    if (strcmp(ota_sub_topic, topic) == 0)
    {

        /* Parse OTA event */
        int err = decode_ota_data(&ota_data, data, data_len);

        /* If error then no update available */
        int result = 0;
        if (err == 0)
        {

            union pyrinas_cloud_version current, incoming;

            /* Compare incoming version with current version */
            ver_from_str(&current, STRINGIFY(PYRINAS_APP_VERSION));
            ver_from_str(&incoming, ota_data.version);

            /*Check numeric*/
            result = ver_comp(&current, &incoming);

            LOG_INF("Version check %s vs %s = %d", STRINGIFY(PYRINAS_APP_VERSION), ota_data.version, result);

            /* If the strings are not equal do stuff*/
            if (result == 0 && strncmp(STRINGIFY(PYRINAS_APP_VERSION), ota_data.version, sizeof(ota_data.version)) != 0)
            {
                result = 1;
            }
        }

        /*If equal and force or if incoming is greater*/
        if (((result == 0 && ota_data.force) || result == 1))
        {

            // Check startup flag. If not set do this
            if (atomic_get(&ota_state_s) == ota_state_ready && atomic_get(&startup_complete_s) == 0)
            {
                LOG_DBG("Start upgrade\n");

                /* Set OTA State */
                atomic_set(&ota_state_s, ota_state_started);

                /* Start upgrade here*/
                k_delayed_work_submit(&fota_work, K_SECONDS(5));

                /* Puase thread */
                k_thread_suspend(pyrinas_cloud_thread_id);
            }
            else
            {
                //If startup flag is is set, reboot.
                sys_reboot(SYS_REBOOT_WARM);
            }
        }
        else
        {

            /* If there wasn't an issue with OTA, continue on our merry way*/
            if (atomic_get(&ota_state_s) == ota_state_ready)
            {
                /*Set the startup complete.*/
                atomic_set(&startup_complete_s, 1);

                /* Let the backend know we're done */
                k_work_submit(&ota_done_work);

                /* Subscribe to Application topic */
                subscribe(application_sub_topic, strlen(application_sub_topic), sys_rand32_get());

                // /* Callback to main to notify complete */
                if (evt_callback)
                {
                    struct pyrinas_cloud_evt evt = {
                        .cloud_state = atomic_get(&cloud_state_s),
                        .ota_state = atomic_get(&ota_state_s)};

                    // Send the callback yo
                    evt_callback(evt);
                }
            }
        }

        return;
    }

    for (int i = 0; i < CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT; i++)
    {
        /* Continue if null */
        if (callbacks[i] == NULL)
            continue;

        /* Determine if this is the topic*/
        if (strncmp(callbacks[i]->full_topic, topic, topic_len) == 0)
        {
            LOG_DBG("Found %s\n", callbacks[i]->topic);

            /* Callbacks to app context */
            callbacks[i]->cb(callbacks[i]->topic, callbacks[i]->topic_len, data, data_len);

            /* Found it,lets break */
            break;
        }
    }
}

/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const c, const struct mqtt_evt *evt)
{
    int err;

    switch (evt->type)
    {
    case MQTT_EVT_CONNACK:
    {
        if (evt->result != 0)
        {
            LOG_ERR("MQTT connect failed %d", evt->result);
            break;
        }

        LOG_INF("MQTT client connected!");

        /* Update state in main context */
        k_work_submit(&state_update_work);

        /* Stop any reconnect attempts */
        k_timer_stop(&reconnect_timer);

        /* On connect work */
        k_work_submit(&on_connect_work);

        /*Start telemetry timer every TELEMETRY_INTERVAL */
        k_timer_start(&telemetry_timer, TELEMETRY_INTERVAL, K_NO_WAIT);

        break;
    }

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("[%s:%d] MQTT client disconnected %d", __func__,
                __LINE__, evt->result);

        /* Stop the timer, we're disconnected*/
        k_timer_stop(&telemetry_timer);

        /* If not forced, try to reconnect */
        if (atomic_get(&cloud_state_s) != cloud_state_force_disconnected)
        {
            /* Set state */
            atomic_set(&cloud_state_s, cloud_state_disconnected);

            /* Update state in main context */
            k_work_submit(&state_update_work);

            /* Set reconnec state */
            atomic_set(&reconnect, 1);

            /* Start reconnect timer*/
            k_timer_start(&reconnect_timer, RECONNECT_INTERVAL, K_NO_WAIT);
        }
        break;

    case MQTT_EVT_PUBLISH:
    {
        const struct mqtt_publish_param *p = &evt->param.publish;

        LOG_DBG("[%s:%d] MQTT PUBLISH result=%d topic=%s len=%d", __func__,
                __LINE__, evt->result, p->message.topic.topic.utf8, p->message.payload.len);
        err = publish_get_payload(c, payload_buf, p->message.payload.len);
        if (err >= 0)
        {
            // Handle the event
            publish_evt_handler(p->message.topic.topic.utf8, p->message.topic.topic.size, payload_buf, p->message.payload.len);
        }
        else
        {
            printk("mqtt_read_publish_payload: Failed! %d\n", err);
            printk("Disconnecting MQTT client...\n");

            err = mqtt_disconnect(c);
            if (err)
            {
                printk("Could not disconnect: %d\n", err);
            }
        }

        // Need to ACK recieved data...
        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE)
        {

            const struct mqtt_puback_param ack = {
                .message_id = p->message_id};

            /* Send acknowledgment. */
            err = mqtt_publish_qos1_ack(c, &ack);
            if (err)
            {
                printk("unable to ack\n");
            }
        }

        break;
    }

    case MQTT_EVT_PUBACK:
        if (evt->result != 0)
            printk("MQTT PUBACK error %d\n", evt->result);

        break;

    case MQTT_EVT_SUBACK:
        if (evt->result != 0)
        {
            printk("MQTT SUBACK error %d\n", evt->result);
            break;
        }

        LOG_DBG("[%s:%d] SUBACK packet id: %u", __func__, __LINE__,
                evt->param.suback.message_id);

        /* If we're subscribed, publish the request */
        if (ota_sub_message_id == evt->param.suback.message_id && atomic_get(&initial_ota_check) == 0)
        {
            /* Set check flag */
            atomic_set(&initial_ota_check, 1);

            /* Submit work */
            k_work_submit(&ota_request_work);
        }

        break;

    default:
        printk("[%s:%d] default: %d\n", __func__, __LINE__, evt->type);
        break;
    }
}

/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
static int broker_init(void)
{
    int err;
    struct addrinfo *result;
    struct addrinfo *addr;
    struct addrinfo hints = {.ai_family = AF_INET,
                             .ai_socktype = SOCK_STREAM};

    err = getaddrinfo(CONFIG_PYRINAS_CLOUD_MQTT_BROKER_HOSTNAME, NULL, &hints, &result);
    if (err)
    {
        printk("ERROR: getaddrinfo failed %d\n", err);
        return err;
    }

    addr = result;
    err = -ENOENT;

    /* Look for address of the broker. */
    while (addr != NULL)
    {
        /* IPv4 Address. */
        if (addr->ai_addrlen == sizeof(struct sockaddr_in))
        {
            struct sockaddr_in *broker4 =
                ((struct sockaddr_in *)&broker);
            char ipv4_addr[NET_IPV4_ADDR_LEN];

            broker4->sin_addr.s_addr =
                ((struct sockaddr_in *)addr->ai_addr)
                    ->sin_addr.s_addr;
            broker4->sin_family = AF_INET;
            broker4->sin_port = htons(CONFIG_PYRINAS_CLOUD_MQTT_BROKER_PORT);

            inet_ntop(AF_INET, &broker4->sin_addr.s_addr, ipv4_addr,
                      sizeof(ipv4_addr));
            LOG_DBG("IPv4 Address found %s", ipv4_addr);

            break;
        }
        else
        {
            printk("ai_addrlen = %u should be %u or %u\n",
                   (unsigned int)addr->ai_addrlen,
                   (unsigned int)sizeof(struct sockaddr_in),
                   (unsigned int)sizeof(struct sockaddr_in6));
        }

        addr = addr->ai_next;
        break;
    }

    /* Free the address. */
    freeaddrinfo(result);

    return 0;
}

static void ota_request_work_fn(struct k_work *unused)
{
    publish_ota_check();
}

static void reboot_work_fn(struct k_work *unused)
{

    LOG_DBG("Reboot work fn\n");

    /* Rebooooot */
    sys_reboot(0);

    /* Rebooting state */
    atomic_set(&ota_state_s, ota_state_rebooting);
}

static void reconnect_work_fn(struct k_work *unused)
{

    int err;

    err = pyrinas_cloud_connect();
    if (err)
    {
        LOG_ERR("Unable to reconnect. Err %d", err);
    }
}

static void ota_done_work_fn(struct k_work *unused)
{
    /* Publish OTA done */
    publish_ota_done();
}

static void state_update_work_fn(struct k_work *unused)
{

    /* Send state event*/
    if (state_event != NULL)
    {
        struct pyrinas_cloud_evt evt = {
            .cloud_state = atomic_get(&cloud_state_s),
            .ota_state = atomic_get(&ota_state_s)};

        state_event(evt);
    }
}

static void work_init()
{
    k_delayed_work_init(&fota_work, fota_start_fn);
    k_delayed_work_init(&disconnect_work, disconnect_work_fn);
    k_work_init(&on_connect_work, on_connect_fn);
    k_work_init(&ota_reboot_work, reboot_work_fn);
    k_work_init(&ota_request_work, ota_request_work_fn);
    k_work_init(&ota_done_work, ota_done_work_fn);
    k_work_init(&reconnect_work, reconnect_work_fn);
    k_work_init(&publish_telemetry_work, publish_telemetry_work_fn);
    k_work_init(&state_update_work, state_update_work_fn);
}

/**@brief Initialize the MQTT client structure
 */
static void client_init(struct mqtt_client *client, char *p_client_id, size_t client_id_sz)
{

    int err;

    /* Initialize workers */
    work_init();

    mqtt_client_init(client);

    err = broker_init();
    assert(err == 0);

    /* MQTT client configuration */
    client->broker = &broker;
    client->evt_cb = mqtt_evt_handler;
    client->client_id.utf8 = p_client_id;
    client->client_id.size = client_id_sz;
    client->password = NULL;
    client->user_name = NULL;
    client->protocol_version = MQTT_VERSION_3_1_1;

    /* MQTT buffers configuration */
    client->rx_buf = rx_buffer;
    client->rx_buf_size = sizeof(rx_buffer);
    client->tx_buf = tx_buffer;
    client->tx_buf_size = sizeof(tx_buffer);

    /* MQTT transport configuration */
    struct mqtt_sec_config *tls_config = &client->transport.tls.config;

    client->transport.type = MQTT_TRANSPORT_SECURE;
    tls_config->peer_verify = CONFIG_PYRINAS_CLOUD_PEER_VERIFY;
    tls_config->cipher_count = 0;
    tls_config->cipher_list = NULL;
    tls_config->sec_tag_count = ARRAY_SIZE(sec_tag_list);
    tls_config->sec_tag_list = sec_tag_list;
    tls_config->hostname = CONFIG_PYRINAS_CLOUD_MQTT_BROKER_HOSTNAME;
}

#ifdef CONFIG_FOTA_DOWNLOAD
static void fota_evt(const struct fota_download_evt *evt)
{

    switch (evt->id)
    {
    case FOTA_DOWNLOAD_EVT_ERROR:
        printk("Received error from fota_download\n");

        /* Set the state */
        atomic_set(&ota_state_s, ota_state_error);

        /* Reboot work start */
        k_work_submit(&ota_reboot_work);

        break;
    case FOTA_DOWNLOAD_EVT_FINISHED:
        printk("OTA Done.\n");

        /* Set the state */
        atomic_set(&ota_state_s, ota_state_done);

        /* Reboot work start */
        k_work_submit(&ota_reboot_work);
        break;

    default:
        break;
    }
}
#endif

int pyrinas_cloud_connect()
{

    int err;

    /* Connect to MQTT */
    err = mqtt_connect(&client);
    if (client.internal.state == 0x4)
    {
        LOG_WRN("Already connected!");

        /* Stop reconnect work */
        k_timer_stop(&reconnect_timer);
    }
    else if (err != 0)
    {
        /* Schedule reconnect work */
        k_timer_start(&reconnect_timer, RECONNECT_INTERVAL, K_SECONDS(30));

        LOG_ERR("mqtt_connect %d", err);
        return err;
    }

    /* Set FDS info */
    fds.fd = client.transport.tls.sock;
    fds.events = POLLIN;

    /* Set state */
    atomic_set(&cloud_state_s, cloud_state_connected);

    /* Start the thread (if not already)*/
    k_sem_reset(&pyrinas_cloud_thread_sem);
    k_sem_give(&pyrinas_cloud_thread_sem);

    return 0;
}

bool pyrinas_cloud_is_connected()
{
    return atomic_get(&cloud_state_s) == cloud_state_connected;
}

int pyrinas_cloud_disconnect()
{
    int err;

    printk("[%s:%d] MQTT client force disconnect!\n", __func__, __LINE__);

    /* Force disconnect flag enabled */
    atomic_set(&cloud_state_s, cloud_state_force_disconnected);

    /* Update state in main context */
    k_work_submit(&state_update_work);

    err = mqtt_disconnect(&client);
    if (err)
    {
        return err;
        printk("Could not disconnect MQTT client. Error: %d\n", err);
    }

    return 0;
}

void pyrinas_cloud_init(pyrinas_cloud_state_evt_t cb)
{
    /*Set the callback*/
    evt_callback = cb;

    /* Get the IMEI */
    get_imei(imei, sizeof(imei));

    /* Print the IMEI */
    LOG_INF("IMEI: %s", imei);

/* Init FOTA client */
#ifdef CONFIG_FOTA_DOWNLOAD
    fota_download_init(fota_evt);
#endif

    /* Set up topics */
    snprintf(ota_pub_topic, sizeof(ota_pub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_PUB_TOPIC, IMEI_LEN, imei);
    snprintf(ota_sub_topic, sizeof(ota_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_SUB_TOPIC, IMEI_LEN, imei);
    snprintf(telemetry_pub_topic, sizeof(telemetry_pub_topic), CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_PUB_TOPIC, IMEI_LEN, imei);
    snprintf(application_sub_topic, sizeof(application_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC, IMEI_LEN, imei, 1, "+");

    /* MQTT client create */
    client_init(&client, imei, sizeof(imei));
}

int pyrinas_cloud_publish_w_uid(char *uid, char *type, uint8_t *data, size_t len)
{

    char topic[256];

    snprintf(topic, sizeof(topic),
             CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_PUB_TOPIC,
             strlen(uid), uid,
             strlen(type), type);

    /* Publish the data */
    int err = data_publish(topic, strlen(topic), data, len, sys_rand32_get());
    if (err)
    {
        LOG_ERR("Unable to publish w/ UID. Error: %d", err);
    }

    return err;
}

int pyrinas_cloud_publish(char *type, uint8_t *data, size_t len)
{

    return pyrinas_cloud_publish_w_uid(imei, type, data, len);
}

void pyrinas_cloud_register_state_evt(pyrinas_cloud_state_evt_t cb)
{
    state_event = cb;
}

void pyrinas_cloud_process()
{
    int err;

    /* Get thread id*/
    pyrinas_cloud_thread_id = k_current_get();

    while (true)
    {
        /* Don't go any further until MQTT is connected */
        k_sem_take(&pyrinas_cloud_thread_sem, K_FOREVER);

        while (atomic_get(&cloud_state_s) == cloud_state_connected)
        {

            /* Then process MQTT */
            err = poll(&fds, 1, mqtt_keepalive_time_left(&client));
            if (err < 0)
            {
                LOG_ERR("ERROR: poll %d\n", errno);
                break;
            }

            err = mqtt_live(&client);
            if (err == 0)
            {
                LOG_INF("[%s:%d] ping sent", __func__, __LINE__);
            }
            else if ((err != 0) && (err != -EAGAIN))
            {
                LOG_ERR("ERROR: mqtt_live %d\n", err);
                break;
            }

            if ((fds.revents & POLLIN) == POLLIN)
            {
                err = mqtt_input(&client);
                if (err != 0)
                {
                    LOG_ERR("ERROR: mqtt_input %d\n", err);
                    break;
                }
            }

            if ((fds.revents & POLLERR) == POLLERR)
            {
                LOG_ERR("POLLERR\n");
                break;
            }

            if ((fds.revents & POLLNVAL) == POLLNVAL)
            {
                LOG_ERR("POLLNVAL\n");
                break;
            }
        }
    }
}

#define THREAD_STACK_SIZE KB(4)
#define PYRINAS_CLOUD_THREAD_PRIORITY (CONFIG_MAIN_THREAD_PRIORITY - 1)
static K_THREAD_STACK_DEFINE(pyrinas_cloud_thread_stack, THREAD_STACK_SIZE);
K_THREAD_DEFINE(pyrinas_cloud_thread, K_THREAD_STACK_SIZEOF(pyrinas_cloud_thread_stack),
                pyrinas_cloud_process, NULL, NULL, NULL,
                PYRINAS_CLOUD_THREAD_PRIORITY, 0, 0);

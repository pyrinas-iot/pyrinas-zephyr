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
// static atomic_t connection_poll_active;

/* Message ID tracking for OTA done */
static uint16_t ota_done_message_id = 0;

/* Atomic flags */
static atomic_val_t cloud_state_s = ATOMIC_INIT(cloud_state_disconnected);
static atomic_val_t ota_state_s = ATOMIC_INIT(ota_state_ready);
static atomic_val_t reconnect = ATOMIC_INIT(0);

/* File descriptor */
static struct pollfd fds;

/* Structures for work */
static struct k_work state_update_work;
static struct k_work publish_telemetry_work;
static struct k_work ota_reboot_work;
static struct k_work ota_done_work;
static struct k_work on_connect_work;
static struct k_work reconnect_work;
static struct k_delayed_work fota_work;

/* Making the ota dat static */
static struct pyrinas_cloud_ota_data ota_data;

/* Callback for application back to main context*/
pryinas_cloud_application_cb_entry_t *callbacks[CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT];

/* Cloud state callback */
static pyrinas_cloud_state_evt_t state_event;

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

            LOG_INF("application subscribe to: %s", callbacks[i]->full_topic);

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
    printk("Reconnect!\n");

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
    printk("Publishing %d bytes to topic: %s len: %u\n", data_len, topic, topic_len);

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
        .list = &subscribe_topic, .list_count = 1, .message_id = 4321};

    printk("Unsubscribing to: %s len %u\n", topic, len);

    return mqtt_unsubscribe(&client, &subscription_list);
}

/**@brief Function to subscribe to the configured topic
 */
static int subscribe(char *topic, size_t len)
{
    struct mqtt_topic subscribe_topic = {
        .topic = {.utf8 = topic,
                  .size = len},
        .qos = MQTT_QOS_1_AT_LEAST_ONCE};

    const struct mqtt_subscription_list subscription_list = {
        .list = &subscribe_topic, .list_count = 1, .message_id = 1234};

    printk("Subscribing to: %s len %u\n", topic, len);

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

    /* Save the message ID to check for an ACK */
    uint16_t message_id = (uint16_t)sys_rand32_get();

    /* Publish the data */
    data_publish(ota_pub_topic, strlen(ota_pub_topic), buf, size, message_id);
}

static void publish_ota_done()
{

    char buf[10];
    size_t size = 0;

    /* Encode the request */
    encode_ota_request(ota_cmd_type_done, buf, sizeof(buf), &size);

    /* Save the message ID to check for an ACK */
    ota_done_message_id = (uint16_t)sys_rand32_get();

    /* Publish the data */
    data_publish(ota_pub_topic, strlen(ota_pub_topic), buf, size, ota_done_message_id);
}

static void publish_telemetry()
{
    char buf[64];
    size_t payload_len = 0;
    encode_telemetry_data(buf, sizeof(buf), &payload_len);
    uint16_t message_id = (uint16_t)sys_rand32_get();
    data_publish(telemetry_pub_topic, strlen(telemetry_pub_topic), buf, payload_len, message_id);
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
    printk("[%s:%d] on connect work function!\n", __func__, __LINE__);

    /* Publish telemetry */
    publish_telemetry();

    /* Trigger OTA check */
    if (atomic_get(&ota_state_s) == ota_state_ready)
    {
        /* Subscribe to OTA topic */
        subscribe(ota_sub_topic, strlen(ota_sub_topic));

        /* Subscribe to Application topic */
        subscribe(application_sub_topic, strlen(application_sub_topic));

        publish_ota_check();
    }
    else if (atomic_get(&ota_state_s) == ota_state_done)
    {
        unsubscribe(ota_sub_topic, strlen(ota_sub_topic));

        publish_ota_done();
    }
}

static void fota_start_fn(struct k_work *unused)
{
    ARG_UNUSED(unused);

    /* Start the FOTA process */
    int err;

    printk("%s/%s using tag %d\n", ota_data.host, ota_data.file, CONFIG_PYRINAS_CLOUD_HTTPS_SEC_TAG);

    /* Start download uses default port and APN*/
    err = fota_download_start(ota_data.host, ota_data.file, CONFIG_PYRINAS_CLOUD_HTTPS_SEC_TAG, 0, NULL);
    if (err)
    {
        printk("fota_download_start error %d\n", err);
        atomic_set(&ota_state_s, ota_state_error);
        return;
    }

    /* Set that we're busy now */
    atomic_set(&ota_state_s, ota_state_downloading);
}

static void publish_evt_handler(const char *topic, size_t topic_len, const char *data, size_t data_len)
{
    printk("topic: %s topic_len: %d data_len: %d\n", topic, topic_len, data_len);

    /* If its the OTA sub topic process */
    if (strcmp(ota_sub_topic, topic) == 0)
    {
        /* Parse OTA event */

        decode_ota_data(&ota_data, data, data_len);
        union pyrinas_cloud_version current, incoming;

        /* Compare incoming version with current version */
        ver_from_str(&current, STRINGIFY(PYRINAS_APP_VERSION));
        ver_from_str(&incoming, ota_data.version);

        int result = ver_comp(&current, &incoming);

        printk("Version check %s vs %s = %d\n", STRINGIFY(PYRINAS_APP_VERSION), ota_data.version, result);

        /*If equal and force or if incoming is greater*/
        if (((result == 0 && ota_data.force) || result == 1))
        {

            if (atomic_get(&ota_state_s) == ota_state_ready)
            {
                printk("Start upgrade\n");

                /* Set OTA State */
                atomic_set(&ota_state_s, ota_state_started);

                /* Start upgrade here*/
                k_delayed_work_submit(&fota_work, K_SECONDS(5));
            }
        }
        else
        {

            /* Cancel ota if the version is not valid */
            k_work_submit(&ota_done_work);
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
            printk("Found %s\n", callbacks[i]->topic);

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
            printk("MQTT connect failed %d\n", evt->result);
            break;
        }

        printk("[%s:%d] MQTT client connected!\n", __func__, __LINE__);

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
        printk("[%s:%d] MQTT client disconnected %d\n", __func__,
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

        printk("[%s:%d] MQTT PUBLISH result=%d topic=%s len=%d\n", __func__,
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
            printk("ack!");

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
        {
            printk("MQTT PUBACK error %d\n", evt->result);
            break;
        }

        printk("[%s:%d] PUBACK packet id: %u\n", __func__, __LINE__,
               evt->param.puback.message_id);

        /* Check if the ack is for OTA done */
        if (ota_done_message_id == evt->param.puback.message_id &&
            atomic_get(&ota_state_s) == ota_state_done)
        {
            printk("[%s:%d] time to rebooot\n", __func__, __LINE__);

            /* Set the state */
            atomic_set(&ota_state_s, ota_state_reboot);

            /* Reboot work start */
            k_work_submit(&ota_reboot_work);
        }

        break;

    case MQTT_EVT_SUBACK:
        if (evt->result != 0)
        {
            printk("MQTT SUBACK error %d\n", evt->result);
            break;
        }

        printk("[%s:%d] SUBACK packet id: %u\n", __func__, __LINE__,
               evt->param.suback.message_id);
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
            printk("IPv4 Address found %s\n", ipv4_addr);

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

static void reboot_work_fn(struct k_work *unused)
{

    printk("Reboot work fn\n");

    /* Disconnect */
    pyrinas_cloud_disconnect();

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
        printk("Unable to reconnect. Err %d", err);
    }
}

static void ota_done_work_fn(struct k_work *unused)
{

    publish_ota_done();
}

static void state_update_work_fn(struct k_work *unused)
{

    /* Send state event*/
    if (state_event != NULL)
        state_event(atomic_get(&cloud_state_s));
}

static void work_init()
{
    k_delayed_work_init(&fota_work, fota_start_fn);
    k_work_init(&on_connect_work, on_connect_fn);
    k_work_init(&ota_reboot_work, reboot_work_fn);
    k_work_init(&ota_done_work, ota_done_work_fn);
    k_work_init(&reconnect_work, reconnect_work_fn);
    k_work_init(&publish_telemetry_work, publish_telemetry_work_fn);
    k_work_init(&state_update_work, state_update_work_fn);
}

/**@brief Initialize the MQTT client structure
 */
static void client_init(struct mqtt_client *client)
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
    client->client_id.utf8 = (uint8_t *)CONFIG_PYRINAS_CLOUD_MQTT_CLIENT_ID;
    client->client_id.size = strlen(CONFIG_PYRINAS_CLOUD_MQTT_CLIENT_ID);
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

        /* Send done message to the cloud */
        publish_ota_done();
        break;

    default:
        break;
    }
}

int pyrinas_cloud_connect()
{

    int err;

    printk("pyrinas_cloud_connect\n");

    /* Connect to MQTT */
    err = mqtt_connect(&client);
    if (err != 0)
    {
        printk("ERROR: mqtt_connect %d\n", err);
        return err;
    }

    /* Set FDS info */
    fds.fd = client.transport.tls.sock;
    fds.events = POLLIN;

    /* Set state */
    atomic_set(&cloud_state_s, cloud_state_connected);

    /* Start the thread */
    k_sem_give(&pyrinas_cloud_thread_sem);

    return 0;
}

void pyrinas_cloud_process()
{
    int err;

    while (true)
    {
        /* Don't go any further until MQTT is connected */
        k_sem_take(&pyrinas_cloud_thread_sem, K_FOREVER);

        printk("starting cloud thread.\n");

        while (atomic_get(&cloud_state_s) == cloud_state_connected)
        {
            /* Then process MQTT */
            err = poll(&fds, 1, mqtt_keepalive_time_left(&client));
            if (err < 0)
            {
                printk("ERROR: poll %d\n", errno);
                break;
            }

            err = mqtt_live(&client);
            if (err == 0)
            {
                printk("[%s:%d] ping sent\n", __func__, __LINE__);
            }
            else if ((err != 0) && (err != -EAGAIN))
            {
                printk("ERROR: mqtt_live %d\n", err);
                break;
            }

            if ((fds.revents & POLLIN) == POLLIN)
            {
                err = mqtt_input(&client);
                if (err != 0)
                {
                    printk("ERROR: mqtt_input %d\n", err);
                    break;
                }
            }

            if ((fds.revents & POLLERR) == POLLERR)
            {
                printk("POLLERR\n");
                break;
            }

            if ((fds.revents & POLLNVAL) == POLLNVAL)
            {
                printk("POLLNVAL\n");
                break;
            }
        }
    }
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

void pyrinas_cloud_init()
{

    /* Get the IMEI */
    get_imei(imei, sizeof(imei));

    /* Print the IMEI */
    printk("imei: %s\n", imei);

    /* Init FOTA client */
    fota_download_init(fota_evt);

    /* Set up topics */
    snprintf(ota_pub_topic, sizeof(ota_pub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_PUB_TOPIC, IMEI_LEN, imei);
    snprintf(ota_sub_topic, sizeof(ota_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_SUB_TOPIC, IMEI_LEN, imei);
    snprintf(telemetry_pub_topic, sizeof(telemetry_pub_topic), CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_PUB_TOPIC, IMEI_LEN, imei);
    snprintf(application_sub_topic, sizeof(application_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC, IMEI_LEN, imei, 1, "+");

    /* MQTT client create */
    client_init(&client);
}

int pyrinas_cloud_publish_w_uid(char *uid, char *type, uint8_t *data, size_t len)
{

    char topic[256];

    snprintf(topic, sizeof(topic),
             CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_PUB_TOPIC,
             strlen(uid), uid,
             strlen(type), type);

    /* Save the message ID to check for an ACK */
    uint16_t message_id = (uint16_t)sys_rand32_get();

    /* Publish the data */
    data_publish(topic, strlen(topic), data, len, message_id);

    return 0;
}

int pyrinas_cloud_publish(char *type, uint8_t *data, size_t len)
{

    return pyrinas_cloud_publish_w_uid(imei, type, data, len);
}

void pyrinas_cloud_register_state_evt(pyrinas_cloud_state_evt_t cb)
{
    state_event = cb;
}

#define THREAD_STACK_SIZE KB(2)
static K_THREAD_STACK_DEFINE(pyrinas_cloud_thread_stack, THREAD_STACK_SIZE);
K_THREAD_DEFINE(pyrinas_cloud_thread, K_THREAD_STACK_SIZEOF(pyrinas_cloud_thread_stack),
                pyrinas_cloud_process, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

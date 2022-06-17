/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include <random/rand32.h>
#include <net/mqtt.h>
#include <net/socket.h>
#include <net/fota_download.h>
#include <assert.h>

#include <pyrinas_cloud/pyrinas_cloud.h>
#include <pyrinas_cloud/pyrinas_version.h>

#include "pyrinas_cloud_codec.h"
#include "pyrinas_cloud_helper.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_cloud);

/*FDS related*/
#define INVALID_FDS -1

/* Security tag for fetching certs */
static sec_tag_t sec_tag_list[] = {CONFIG_PYRINAS_CLOUD_SEC_TAG};

/* Buffers for MQTT client. */
static uint8_t rx_buffer[CONFIG_PYRINAS_CLOUD_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_PYRINAS_CLOUD_MQTT_MESSAGE_BUFFER_SIZE];

/* Topics */
char ota_pub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_OTA_PUB_TOPIC) + IMEI_LEN];
char ota_sub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_OTA_SUB_TOPIC) + IMEI_LEN];
char telemetry_pub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_PUB_TOPIC) + IMEI_LEN];
char application_sub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC) + IMEI_LEN + CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE];

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Thread control */
static K_SEM_DEFINE(connection_poll_sem, 0, 1);

/* Atomic flags */
static atomic_val_t cloud_state_s = ATOMIC_INIT(cloud_state_disconnected);
static atomic_val_t ota_state_s = ATOMIC_INIT(ota_state_ready);
static atomic_val_t initial_ota_check = ATOMIC_INIT(0);
static atomic_t connection_poll_active;

/* File descriptor */
static struct pollfd fds;

/* Structures for work */
static struct k_work ota_request_work;
static struct k_work ota_done_work;
static struct k_work on_connect_work;
static struct k_work_delayable ota_check_subscribed_work;
static struct k_work_delayable fota_work;

/* Making the ota dat static */
static struct pyrinas_cloud_ota_package ota_package;

/* Callback for application back to main context*/
pryinas_cloud_application_cb_entry_t callbacks[CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT];

/* Cloud state callbacks */
static struct pyrinas_cloud_config m_config;

/* Statically track message id*/
static uint16_t ota_sub_message_id = 0;

/* Stack definition for application workqueue */
K_THREAD_STACK_DEFINE(cloud_stack_area,
                      CONFIG_PYRINAS_CLOUD_WORKQUEUE_STACK_SIZE);
static struct k_work_q cloud_work_q;

/* Username and password */
struct mqtt_utf8 mqtt_user_name, mqtt_password;

/* Track message id*/
static uint16_t message_id = 1;

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

    /* Return if name is greater than name size */
    if (topic_len > CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE)
    {
        return -EINVAL;
    }

    for (int i = 0; i < CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT; i++)
    {
        if (callbacks[i].cb == NULL)
        {

            /* Set the full name */
            snprintf(callbacks[i].full_topic, sizeof(application_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC, m_config.client_id.len, m_config.client_id.str, topic_len, topic);

            LOG_DBG("application subscribe to: %s", callbacks[i].full_topic);

            /* Set the values */
            callbacks[i].cb = callback;
            callbacks[i].topic_len = topic_len;

            /* Copy the name*/
            memcpy(callbacks[i].topic, topic, topic_len);

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
        if (callbacks[i].cb == NULL)
            continue;

        if (strcmp(callbacks[i].topic, topic) == 0)
        {
            callbacks[i].cb = NULL;
            return 0;
        }
    }

    /* Entry not found */
    return -ENOENT;
}

/**@brief Function to publish data on the configured topic
 */
static int data_publish(uint8_t *topic, size_t topic_len, uint8_t *data, size_t data_len, uint16_t message_id)
{

    LOG_DBG("Pub to %.*s", topic_len, topic);

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

    LOG_INF("Publishing %d bytes to topic: %.*s", data_len, topic_len, log_strdup(topic));

    int err = mqtt_publish(&client, &param);

    return err;
}

/**@brief Function to unsubscribe to the configured topic
static int unsubscribe(char *topic, size_t len)
{
    struct mqtt_topic subscribe_topic = {
        .topic = {.utf8 = topic,
                  .size = len},
        .qos = MQTT_QOS_1_AT_LEAST_ONCE};

    const struct mqtt_subscription_list subscription_list = {
        .list = &subscribe_topic, .list_count = 1, .message_id = message_id++};

    printk("Unsubscribing to: %s len %u\n", topic, len);

    return mqtt_unsubscribe(&client, &subscription_list);
}
 */

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

    LOG_INF("Subscribing to topic: %s", log_strdup(topic));

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

    if (length > CONFIG_PYRINAS_CLOUD_MQTT_PAYLOAD_SIZE)
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
    int err = data_publish(ota_pub_topic, strlen(ota_pub_topic), buf, size, message_id++);
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
    int err = data_publish(ota_pub_topic, strlen(ota_pub_topic), buf, size, message_id++);
    if (err)
    {
        LOG_ERR("Unable to publish OTA done. Error: %d", err);
    }
}

/* Publish central/hub telemetry */
int pyrinas_cloud_publish_telemetry(struct pyrinas_cloud_telemetry_data *p_telemetry_data)
{

    int err;
    size_t data_size = 0;
    char data[CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_SIZE];

    /* Encode data */
    err = encode_telemetry_data(p_telemetry_data, data, sizeof(data), &data_size);
    if (err)
    {
        LOG_ERR("Unable to encode telemetry data.");
        return err;
    }

    /* Publish it */
    return data_publish(telemetry_pub_topic, strlen(telemetry_pub_topic), data, data_size, message_id++);
}

static void on_connect_fn(struct k_work *unused)
{
    LOG_DBG("[%s:%d] on connect work function!", __func__, __LINE__);

    /* Data */
    struct pyrinas_cloud_telemetry_data data = {
        .has_version = true,
    };

    /* Get version string */
    strncpy(data.version, CONFIG_PYRINAS_APP_VERSION, sizeof(data.version));

    /* Publish telemetry */
    int err = pyrinas_cloud_publish_telemetry(&data);
    if (err)
    {
        LOG_ERR("Error publishing telemetry: %i", err);
    }

    /* Trigger OTA check */
    if (atomic_get(&ota_state_s) == ota_state_ready)
    {

        /* Save this for later */
        ota_sub_message_id = (uint16_t)message_id++;

        /* Subscribe to OTA topic */
        subscribe(ota_sub_topic, strlen(ota_sub_topic), ota_sub_message_id);
    }
}

static void ota_check_subscribed_work_fn(struct k_work *unused)
{

    /* Check if, after a certain amount of time, no response has been had. If no, assert and reboot*/
    __ASSERT(atomic_get(&initial_ota_check) == 1, "No response from server!");
}

static void fota_start_fn(struct k_work *unused)
{
    ARG_UNUSED(unused);

#ifdef CONFIG_FOTA_DOWNLOAD

    /* Start the FOTA process */
    int err;
    int sec_tag = -1;

    /* Set the security tag if TLS is enabled. */
#if defined(CONFIG_PYRINAS_CLOUD_HTTPS_SEC_TAG)
    sec_tag = CONFIG_PYRINAS_CLOUD_HTTPS_SEC_TAG;
#endif

    int index = -1;

    /* Make sure we're using the correct image*/
    for (int i = 0; i < PYRINAS_OTA_PACKAGE_MAX_FILE_COUNT; i++)
    {
        // Set the index
        if (ota_package.files[i].image_type == pyrinas_cloud_ota_image_type_primary)
        {
            index = i;
            break;
        }
    }

    /* If a primary image is not found error! */
    if (index == -1)
    {
        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
                                            .data.err = -ENOTSUP};
            m_config.evt_cb(&evt);
        }

        return;
    }

    LOG_DBG("%s/%s using tag %d", ota_package.files[index].host, ota_package.files[index].file, sec_tag);

    /* Start download uses default port and APN*/
    err = fota_download_start(ota_package.files[index].host, ota_package.files[index].file, sec_tag, 0, 0);
    if (err)
    {
        LOG_ERR("fota_download_start error %d", err);
        atomic_set(&ota_state_s, ota_state_error);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
                                            .data.err = err};
            m_config.evt_cb(&evt);
        }

        return;
    }

    /* Set that we're busy now */
    atomic_set(&ota_state_s, ota_state_downloading);

    /* Send to calback */
    if (m_config.evt_cb)
    {
        struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_DOWNLOADING};
        m_config.evt_cb(&evt);
    }
#endif
}

static void publish_sort(struct pyrinas_cloud_evt *message)
{

    int result = 0;

    /* If its the OTA sub topic process */
    if (strncmp(ota_sub_topic, message->data.msg.topic, message->data.msg.topic_len) == 0)
    {
        LOG_DBG("Found %s. Data: %s Data size: %d", ota_sub_topic, message->data.msg.data, message->data.msg.data_len);

        /* Set check flag */
        atomic_set(&initial_ota_check, 1);

        /* Reset ota_package conents */
        memset(&ota_package, 0, sizeof(ota_package));

        /* Parse OTA event */
        int err = decode_ota_package(&ota_package, message->data.msg.data, message->data.msg.data_len);

        /* If error then no update available */
        if (err == 0)
        {

            /* Check numeric */
            result = ver_comp(&pyrinas_version, &ota_package.version);

            /* Print result */
            LOG_INF("ota: New version? %s ", result == 1 ? "true" : "false");
            LOG_INF("ota: Remote version: %i.%i.%i-%i ", ota_package.version.major, ota_package.version.minor, ota_package.version.patch, ota_package.version.commit);
        }
        else
        {
            LOG_WRN("ota: Unable to decode OTA data");
        }

        /* If incoming is greater or hash is not equal */
        if (result == 1)
        {

            LOG_INF("ota: Start upgrade");

            /* Set OTA State */
            atomic_set(&ota_state_s, ota_state_started);

            /* Send to calback */
            if (m_config.evt_cb)
            {
                struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_START};
                m_config.evt_cb(&evt);
            }

            /* Start upgrade here*/
            k_work_schedule_for_queue(&cloud_work_q, &fota_work, K_SECONDS(5));
        }
        else
        {

            LOG_INF("ota: No update.");

            /* If there wasn't an issue with OTA, continue on our merry way*/
            if (atomic_get(&ota_state_s) == ota_state_ready)
            {
                LOG_INF("ota: Ota ready.");

                /* Let the backend know we're done */
                k_work_submit_to_queue(&cloud_work_q, &ota_done_work);

                /* Subscribe to Application topic */
                subscribe(application_sub_topic, strlen(application_sub_topic), message_id++);

                /* Send to calback */
                if (m_config.evt_cb)
                {
                    struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_READY};
                    m_config.evt_cb(&evt);
                }
            }
        }

        return;
    }

    for (int i = 0; i < CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT; i++)
    {
        /* Continue if null */
        if (callbacks[i].cb == NULL)
            continue;

        LOG_DBG("%s %d", message->data.msg.data, message->data.msg.topic_len);

        /* Determine if this is the topic*/
        if (strncmp(callbacks[i].full_topic, message->data.msg.topic, message->data.msg.topic_len) == 0)
        {
            LOG_DBG("Found %s", callbacks[i].topic);

            /* Callbacks to app context */
            callbacks[i].cb(callbacks[i].topic, callbacks[i].topic_len, message->data.msg.data, message->data.msg.data_len);

            /* Found it,lets break */
            break;
        }
    }

    /* Send to calback */
    if (m_config.evt_cb)
    {
        m_config.evt_cb(message);
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

            /* Not connected */
            atomic_set(&cloud_state_s, cloud_state_disconnected);

            break;
        }

        /* Connected */
        atomic_set(&cloud_state_s, cloud_state_connected);

        /* Set OTA State back to Ready state */
        atomic_set(&ota_state_s, ota_state_ready);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_CONNECTED};
            m_config.evt_cb(&evt);
        }

        /* On connect work */
        k_work_submit_to_queue(&cloud_work_q, &on_connect_work);

        break;
    }

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("[%s:%d] MQTT client disconnected %d", __func__,
                __LINE__, evt->result);

        /* Set state */
        atomic_set(&cloud_state_s, cloud_state_disconnected);

        /* Clear check flag */
        atomic_set(&initial_ota_check, 0);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_DISCONNECTED};
            m_config.evt_cb(&evt);
        }

        break;

    case MQTT_EVT_PUBLISH:
    {
        const struct mqtt_publish_param *p = &evt->param.publish;

        /*Ensure topic length isn't over max size */
        if (p->message.payload.len > CONFIG_PYRINAS_CLOUD_MQTT_PAYLOAD_SIZE ||
            p->message.topic.topic.size > CONFIG_PYRINAS_CLOUD_MQTT_TOPIC_SIZE)
        {
            LOG_ERR("Payload error");
            break;
        }

        struct pyrinas_cloud_evt message = {
            .type = PYRINAS_CLOUD_EVT_DATA_RECIEVED,
            .data = {
                .msg = {
                    .data_len = p->message.payload.len,
                    .topic_len = p->message.topic.topic.size}}};

        /* Copy topic */
        memcpy(message.data.msg.topic, p->message.topic.topic.utf8, p->message.topic.topic.size);

        LOG_DBG("[%s:%d] MQTT PUBLISH result=%d topic=%s len=%d", __func__,
                __LINE__, evt->result, log_strdup(p->message.topic.topic.utf8), p->message.payload.len);

        err = publish_get_payload(c, message.data.msg.data, p->message.payload.len);
        if (err >= 0)
        {

            /* Sor and push message via callback  */
            publish_sort(&message);
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

        /* Need to ACK recieved data... */
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
            LOG_ERR("MQTT PUBACK error %d", evt->result);

        break;

    case MQTT_EVT_SUBACK:
        if (evt->result != 0)
        {
            LOG_ERR("MQTT SUBACK error %d", evt->result);
            break;
        }

        LOG_DBG("[%s:%d] SUBACK packet id: %u", __func__, __LINE__,
                evt->param.suback.message_id);

        /* If we're subscribed, publish the request */
        if (ota_sub_message_id == evt->param.suback.message_id && atomic_get(&initial_ota_check) == 0)
        {
            /* Submit work */
            k_work_submit_to_queue(&cloud_work_q, &ota_request_work);

            /* Delay work to make sure we are *at least* subscribed to OTA*/
            k_work_schedule_for_queue(&cloud_work_q, &ota_check_subscribed_work, K_SECONDS(5));
        }

        break;

    default:
        LOG_DBG("[%s:%d] default: %d", __func__, __LINE__, evt->type);
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
        return err;

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

static void ota_done_work_fn(struct k_work *unused)
{
    /* Publish OTA done */
    publish_ota_done();
}

static void work_init()
{

    k_work_queue_start(&cloud_work_q, cloud_stack_area,
                       K_THREAD_STACK_SIZEOF(cloud_stack_area),
                       CONFIG_PYRINAS_CLOUD_WORKQUEUE_PRIORITY, NULL);

    k_work_init_delayable(&fota_work, fota_start_fn);
    k_work_init_delayable(&ota_check_subscribed_work, ota_check_subscribed_work_fn);
    k_work_init(&on_connect_work, on_connect_fn);
    k_work_init(&ota_request_work, ota_request_work_fn);
    k_work_init(&ota_done_work, ota_done_work_fn);
}

/**@brief Initialize the MQTT client structure
 */
static int client_init(struct mqtt_client *client, char *p_client_id, size_t client_id_sz)
{

    int err;

    mqtt_client_init(client);

    err = broker_init();
    if (err != 0)
        return err;

    /* MQTT client configuration */
    client->broker = &broker;
    client->evt_cb = mqtt_evt_handler;
    client->client_id.utf8 = p_client_id;
    client->client_id.size = client_id_sz;
    client->protocol_version = MQTT_VERSION_3_1_1;

/* MQTT user name and password */
#ifdef CONFIG_PYRINAS_CLOUD_PASSWORD
    mqtt_password.utf8 = CONFIG_PYRINAS_CLOUD_PASSWORD;
    mqtt_password.size = strlen(CONFIG_PYRINAS_CLOUD_PASSWORD);
    client->password = &mqtt_password;
#else
    client->password = NULL;
#endif

#ifdef CONFIG_PYRINAS_CLOUD_USER_NAME
    mqtt_user_name.utf8 = CONFIG_PYRINAS_CLOUD_USER_NAME;
    mqtt_user_name.size = strlen(CONFIG_PYRINAS_CLOUD_USER_NAME);
    client->user_name = &mqtt_user_name;
#else
    client->user_name = NULL;
#endif

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
    tls_config->session_cache = TLS_SESSION_CACHE_DISABLED;

    return 0;
}

#ifdef CONFIG_FOTA_DOWNLOAD
static void fota_evt(const struct fota_download_evt *p_evt)
{

    switch (p_evt->id)
    {
    case FOTA_DOWNLOAD_EVT_ERROR:
        LOG_INF("Received error from fota_download");

        /* Set the state */
        atomic_set(&ota_state_s, ota_state_error);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_ERROR,
                                            .data.err = (int)p_evt->cause};
            m_config.evt_cb(&evt);
        }

        /* Disconnect */
        pyrinas_cloud_disconnect();

        break;
    case FOTA_DOWNLOAD_EVT_FINISHED:
        LOG_INF("OTA Done.");

        /* Set the state */
        atomic_set(&ota_state_s, ota_state_done);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_DONE};
            m_config.evt_cb(&evt);
        }

        break;

    default:
        break;
    }
}
#endif

int pyrinas_cloud_connect()
{

    /* Check if thread is already active. */
    if (atomic_get(&connection_poll_active))
    {
        LOG_WRN("Connection poll in progress");
        return -EINPROGRESS;
    }

    /* Start the thread (if not already)*/
    k_sem_give(&connection_poll_sem);

    return 0;
}

bool pyrinas_cloud_is_connected()
{
    return atomic_get(&cloud_state_s) == cloud_state_connected;
}

int pyrinas_cloud_disconnect()
{
    int err;

    err = mqtt_disconnect(&client);
    if (err)
    {
        return err;
        LOG_ERR("Could not disconnect MQTT client. Error: %d", err);
    }

    return 0;
}

int pyrinas_cloud_init(struct pyrinas_cloud_config *p_config)
{
    int err;

    /* For debug purposes */
    LOG_INF("Connecting to: %s on port %d", CONFIG_PYRINAS_CLOUD_MQTT_BROKER_HOSTNAME, CONFIG_PYRINAS_CLOUD_MQTT_BROKER_PORT);

    /*Set the callback*/
    m_config = *p_config;

/* Init FOTA client */
#ifdef CONFIG_FOTA_DOWNLOAD
    fota_download_init(fota_evt);
#endif

    /* Set up topics */
    snprintf(ota_pub_topic, sizeof(ota_pub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_PUB_TOPIC, p_config->client_id.len, p_config->client_id.str);
    snprintf(ota_sub_topic, sizeof(ota_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_SUB_TOPIC, p_config->client_id.len, p_config->client_id.str);
    snprintf(telemetry_pub_topic, sizeof(telemetry_pub_topic), CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_PUB_TOPIC, p_config->client_id.len, p_config->client_id.str);
    snprintf(application_sub_topic, sizeof(application_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC, p_config->client_id.len, p_config->client_id.str, 1, "+");

    /* Initialize workers */
    work_init();

    /* MQTT client create */
    err = client_init(&client, p_config->client_id.str, p_config->client_id.len);
    if (err != 0)
    {
        LOG_ERR("client_init %d", err);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_ERROR,
                                            .data.err = err};
            m_config.evt_cb(&evt);
        }

        return err;
    }

    return 0;
}

int pyrinas_cloud_publish_evt_telemetry(pyrinas_event_t *evt)
{

    char uid[14];
    int err = 0;

    size_t data_len = 0;
    uint8_t data[CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_SIZE];
    uint8_t topic[CONFIG_PYRINAS_CLOUD_MQTT_TOPIC_SIZE];

    struct pyrinas_cloud_telemetry_data telemetry_data;

    /* Memset uid */
    memset(uid, 0, sizeof(uid));

    /* Set default off*/
    telemetry_data.has_version = false;
    telemetry_data.has_rsrp = false;

    /* Check if central RSSI */
    /*
    if (evt->central_rssi < 0)
    {
        data.has_central_rssi = true;
        data.central_rssi = evt->central_rssi;
    }
    */
    /* TODO: address the fact that this is not generated! */
    telemetry_data.has_central_rssi = false;

    /* Check if peripheral RSSI */
    if (evt->peripheral_rssi < 0)
    {
        telemetry_data.has_peripheral_rssi = true;
        telemetry_data.peripheral_rssi = evt->peripheral_rssi;
    }

    LOG_DBG("Rssi: %i %i", telemetry_data.central_rssi, telemetry_data.peripheral_rssi);

    /* Encode data */
    err = encode_telemetry_data(&telemetry_data, data, sizeof(data), &data_len);
    if (err)
    {
        LOG_ERR("Unable to encode telemetry data.");
        return err;
    }

    /* Get peripheral address */
    err = snprintf(uid, sizeof(uid), "%02x%02x%02x%02x%02x%02x",
                   evt->peripheral_addr[0], evt->peripheral_addr[1], evt->peripheral_addr[2],
                   evt->peripheral_addr[3], evt->peripheral_addr[4], evt->peripheral_addr[5]);

    if (err < 0)
    {
        LOG_ERR("Unable to create UID. Err: %i", err);
        return err;
    }

    /* Create topic */
    int ret = snprintf(topic, sizeof(topic),
                       CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_PUB_TOPIC,
                       strlen(uid), uid);
    if (ret < 0)
    {
        LOG_ERR("Unable to populate telemetry topic. Err: %i", ret);
        return ret;
    }

    /* Publish it */
    return data_publish(topic, ret, data, data_len, message_id++);
}

int pyrinas_cloud_publish_evt(pyrinas_event_t *evt)
{

    char uid[14];
    char topic[CONFIG_PYRINAS_CLOUD_MQTT_TOPIC_SIZE];
    int ret;

    /* Memset uid */
    memset(uid, 0, sizeof(uid));

    /* Get peripheral address */
    ret = snprintf(uid, sizeof(uid), "%02x%02x%02x%02x%02x%02x",
                   evt->peripheral_addr[0], evt->peripheral_addr[1], evt->peripheral_addr[2],
                   evt->peripheral_addr[3], evt->peripheral_addr[4], evt->peripheral_addr[5]);
    if (ret < 0)
    {
        LOG_ERR("Unable to create UID. Err: %i", ret);
        return ret;
    }

    LOG_DBG("Sending from [%s] %s.", log_strdup(uid), log_strdup(evt->name.bytes));

    /* Create topic */
    ret = snprintf(topic, sizeof(topic),
                   CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_PUB_TOPIC,
                   strlen(uid), uid,
                   evt->name.size, evt->name.bytes);
    if (ret < 0)
    {
        LOG_ERR("Unable to populate topic. Err: %i", ret);
        return ret;
    }

    /* Publish it */
    return data_publish(topic, ret, evt->data.bytes, evt->data.size, message_id++);
}

int pyrinas_cloud_publish(char *type, uint8_t *data, size_t len)
{
    int ret;
    char topic[CONFIG_PYRINAS_CLOUD_MQTT_TOPIC_SIZE];

    /* Create topic */
    ret = snprintf(topic, sizeof(topic),
                   CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_PUB_TOPIC,
                   m_config.client_id.len, m_config.client_id.str,
                   strlen(type), type);

    if (ret < 0)
    {
        LOG_ERR("Unable to populate telemetry topic. Err: %i", ret);
        return ret;
    }

    /* Publish it */
    data_publish(topic, ret, data, len, message_id++);

    return 0;
}

void pyrinas_cloud_poll(void)
{
    int err;

start:
    k_sem_take(&connection_poll_sem, K_FOREVER);
    atomic_set(&connection_poll_active, 1);

    /* Connect to MQTT */
    err = mqtt_connect(&client);
    if (err != 0)
    {
        LOG_ERR("mqtt_connect %d", err);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_ERROR};
            m_config.evt_cb(&evt);
        }

        goto reset;
    }

    struct timeval timeout = {
        .tv_sec = 60};

    /* Set FDS info */
    fds.fd = client.transport.tcp.sock;
    fds.events = POLLIN;

    /* Set timeout for sening data */
    err = setsockopt(fds.fd, SOL_SOCKET, SO_SNDTIMEO,
                     &timeout, sizeof(timeout));
    if (err == -1)
    {
        LOG_ERR("Failed to set timeout, errno: %d", errno);
    }
    else
    {
        LOG_INF("Using socket send timeout of %d seconds",
                (uint32_t)timeout.tv_sec);
    }

    while (true)
    {

        /* Then process MQTT */
        err = poll(&fds, 1, mqtt_keepalive_time_left(&client));

        /* If poll returns 0 the timeout has expired. */
        if (err == 0)
        {
            mqtt_input(&client);
            mqtt_live(&client);
            continue;
        }

        if ((fds.revents & POLLIN) == POLLIN)
        {
            mqtt_input(&client);
            mqtt_live(&client);

            /* Check if socket is closed */
            if (atomic_get(&cloud_state_s) == cloud_state_disconnected)
            {
                LOG_DBG("Socket already closed!");
                break;
            }

            continue;
        }

        if (err < 0)
        {
            LOG_ERR("ERROR: poll %d", errno);
            break;
        }

        if ((fds.revents & POLLERR) == POLLERR)
        {
            LOG_WRN("POLLERR");
            break;
        }

        if ((fds.revents & POLLHUP) == POLLHUP)
        {
            LOG_WRN("POLLHUP");
            break;
        }

        if ((fds.revents & POLLNVAL) == POLLNVAL)
        {
            LOG_WRN("POLLNVAL");
            break;
        }
    }

reset:

    /* Push event */
    if (atomic_get(&cloud_state_s) != cloud_state_disconnected)
    {
        atomic_set(&cloud_state_s, cloud_state_disconnected);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_DISCONNECTED};
            m_config.evt_cb(&evt);
        }
    }

    /* Reset thread */
    atomic_set(&connection_poll_active, 0);
    k_sem_take(&connection_poll_sem, K_NO_WAIT);
    goto start;
}

#define PYRINAS_CLOUD_THREAD_STACK_SIZE KB(4)
K_THREAD_DEFINE(pyrinas_cloud_thread, PYRINAS_CLOUD_THREAD_STACK_SIZE,
                pyrinas_cloud_poll, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/*
 * Copyright (c) 2021 Circuit Dojo LLC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include <random/rand32.h>
#include <net/mqtt.h>
#include <net/socket.h>
#include <assert.h>
#include <settings/settings.h>

#include <pyrinas_cloud/pyrinas_cloud.h>
#include <pyrinas_cloud/pyrinas_cloud_codec.h>

#ifdef CONFIG_PYRINAS_CLOUD_OTA_ENABLED
#include <pyrinas_cloud/pyrinas_cloud_ota.h>
#endif

#include <logging/log.h>
LOG_MODULE_REGISTER(pyrinas_cloud);

/*FDS related*/
#define INVALID_FDS -1

/* Security tag for fetching certs */
static sec_tag_t sec_tag_list[] = {CONFIG_PYRINAS_CLOUD_SEC_TAG};

/* Cloud credentials */
static uint8_t cloud_credentials[3][1024];

/* Buffers for MQTT client. */
static uint8_t rx_buffer[CONFIG_PYRINAS_CLOUD_MQTT_MESSAGE_BUFFER_SIZE] = {0};
static uint8_t tx_buffer[CONFIG_PYRINAS_CLOUD_MQTT_MESSAGE_BUFFER_SIZE] = {0};

/* Topics */
#ifdef CONFIG_PYRINAS_CLOUD_OTA_ENABLED
char ota_sub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_OTA_TOPIC) + PYRINAS_DEV_ID_LEN + 1] = {0};
char ota_download_sub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_OTA_DOWNLOAD_TOPIC) + PYRINAS_DEV_ID_LEN + 1] = {0};
#endif
char telemetry_pub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_PUB_TOPIC) + PYRINAS_DEV_ID_LEN] = {0};
char application_sub_topic[sizeof(CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC) + PYRINAS_DEV_ID_LEN + CONFIG_PYRINAS_CLOUD_APPLICATION_EVENT_NAME_MAX_SIZE] = {0};

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Thread control */
static K_SEM_DEFINE(connection_poll_sem, 0, 1);

/* Atomic flags */
static atomic_val_t cloud_state_s = ATOMIC_INIT(cloud_state_disconnected);
static atomic_t connection_poll_active;

/* File descriptor */
static struct pollfd fds;

#ifdef CONFIG_PYRINAS_CLOUD_OTA_ENABLED
/* Atomic flags */
static atomic_val_t initial_ota_check = ATOMIC_INIT(0);

/* Statically track message id*/
static uint16_t ota_sub_message_id = 0;
#endif

/* Callback for application back to main context*/
pryinas_cloud_application_cb_entry_t callbacks[CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT];

/* Cloud state callbacks */
static struct pyrinas_cloud_config m_config;

/* Username and password */
struct mqtt_utf8 mqtt_user_name, mqtt_password;

/* Track message id*/
static uint16_t message_id = 1;

/* Is init */
bool is_init = false;

/* Memory for username, password, port and hostname */
static char m_username[128] = {0};
static char m_password[128] = {0};
static char m_port[8] = {0};
static char m_hostname[128] = {0};

/* Static message */
struct pyrinas_cloud_evt message;

/* Forward declarations */
static int settings_read_callback(const char *key,
                                  size_t len,
                                  settings_read_cb read_cb,
                                  void *cb_arg,
                                  void *param);
static void pyrinas_rx_process(struct pyrinas_cloud_evt *p_message);

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

            LOG_INF("Application subscribe to: %s", (char*)callbacks[i].full_topic);

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

int pyrinas_cloud_publish_raw(uint8_t *topic, size_t topic_len, uint8_t *data, size_t data_len)
{
    LOG_DBG("Pub to %.*s", topic_len, (char *)topic);

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
    param.message_id = message_id++;
    param.dup_flag = 0;
    param.retain_flag = 0;

    LOG_DBG("Publishing %d bytes to topic: %.*s", data_len, topic_len, (char *)topic);

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

    LOG_ERR("Unsubscribing to: %s len %u", topic, len);

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

    LOG_INF("Subscribing to topic: %s", (char *)topic);

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

/* Publish central/hub telemetry */
int pyrinas_cloud_publish_telemetry(struct pyrinas_cloud_telemetry_data *p_telemetry_data)
{
    int err;
    size_t data_size = 0;
    char data[CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_SIZE] = {0};

    /* Encode data */
    err = encode_telemetry_data(p_telemetry_data, data, sizeof(data), &data_size);
    if (err)
    {
        LOG_ERR("Unable to encode telemetry data.");
        return err;
    }

    /* Publish it */
    return pyrinas_cloud_publish_raw(telemetry_pub_topic, strlen(telemetry_pub_topic), data, data_size);
}

int pyrinas_cloud_on_connect()
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
        return err;
    }

#ifdef CONFIG_PYRINAS_CLOUD_OTA_ENABLED
    /* Save this for later */
    ota_sub_message_id = (uint16_t)message_id++;

    /* Subscribe to OTA topic */
    subscribe(ota_sub_topic, strlen(ota_sub_topic), ota_sub_message_id);
    subscribe(ota_download_sub_topic, strlen(ota_download_sub_topic), message_id++);

#endif
    /* Subscribe to Application topic */
    subscribe(application_sub_topic, strlen(application_sub_topic), message_id++);

    return 0;
}

/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const client, const struct mqtt_evt *evt)
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

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_CONNECTED};
            m_config.evt_cb(&evt);
        }

        break;
    }

    case MQTT_EVT_DISCONNECT:
        LOG_WRN("[%s:%d] MQTT client disconnected %d", __func__,
                __LINE__, evt->result);

        /* Set state */
        atomic_set(&cloud_state_s, cloud_state_disconnected);

#ifdef CONFIG_PYRINAS_CLOUD_OTA_ENABLED
        /* Clear check flag */
        atomic_set(&initial_ota_check, 0);
#endif

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

        message.type = PYRINAS_CLOUD_EVT_DATA_RECEIVED;
        message.data.data_len = p->message.payload.len;
        message.data.topic_len = p->message.topic.topic.size;

        /* Copy topic */
        memcpy(message.data.topic, p->message.topic.topic.utf8, p->message.topic.topic.size);

        LOG_DBG("[%s:%d] MQTT PUBLISH result=%d topic=%s len=%d", __func__,
                __LINE__, evt->result, p->message.topic.topic.utf8, p->message.payload.len);

        err = publish_get_payload(client, message.data.data, message.data.data_len);
        if (err)
        {
            LOG_ERR("mqtt_read_publish_payload: Failed! %d", err);
        }
        else
        {
            /* Sort and push message via callback  */
            pyrinas_rx_process(&message);
        }

        /* Need to ACK recieved data... */
        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE)
        {
            struct mqtt_puback_param puback = {
                .message_id = p->message_id};

            mqtt_publish_qos1_ack(client, &puback);
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

#ifdef CONFIG_PYRINAS_CLOUD_OTA_ENABLED
        /* If we're subscribed, publish the request */
        if (ota_sub_message_id == evt->param.suback.message_id && atomic_get(&initial_ota_check) == 0)
        {
            /* Send to calback */
            if (m_config.evt_cb)
            {
                struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_FOTA_READY};
                m_config.evt_cb(&evt);
            }
        }
#endif

        break;

    default:
        LOG_DBG("[%s:%d] default: %d", __func__, __LINE__, evt->type);
        break;
    }
}

/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
static int broker_init(char *hostname, uint16_t port)
{
    int err;
    struct addrinfo *result;
    struct addrinfo *addr;
    struct addrinfo hints = {.ai_family = AF_INET,
                             .ai_socktype = SOCK_STREAM};

    err = getaddrinfo(hostname, NULL, &hints, &result);
    if (err)
    {
        LOG_ERR("Unable to get address.");
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
            broker4->sin_port = htons(port);

            inet_ntop(AF_INET, &broker4->sin_addr.s_addr, ipv4_addr,
                      sizeof(ipv4_addr));
            LOG_DBG("IPv4 Address found %s", ipv4_addr);

            break;
        }
        else
        {
            LOG_ERR("ai_addrlen = %u should be %u or %u",
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

static int settings_read_callback(const char *key,
                                  size_t len,
                                  settings_read_cb read_cb,
                                  void *cb_arg,
                                  void *param)
{
    ssize_t num_read_bytes = 0;
    struct pyrinas_cloud_settings_params *params = param;

    /* Make sure this is false first */
    params->found = false;

    /* Process only the exact match and ignore descendants of the searched name */
    if (settings_name_next(key, NULL) != 0)
    {
        return 0;
    }

    /* Mark found */
    params->found = true;

    /* Get num bytes to read*/
    num_read_bytes = MIN(len, params->len);

    /* Read! */
    num_read_bytes = read_cb(cb_arg, params->buf, num_read_bytes);

    /* Set bytes read.. */
    params->len = num_read_bytes;

    return 0;
}

/**@brief Initialize the MQTT client structure
 */
static int client_init(struct pyrinas_cloud_client_init *init, struct mqtt_client *client, char *p_client_id, size_t client_id_sz)
{
    int err;

    mqtt_client_init(client);

    err = broker_init(init->hostname, init->port);
    if (err != 0)
        return err;

    /* MQTT client configuration */
    client->broker = &broker;
    client->evt_cb = mqtt_evt_handler;
    client->client_id.utf8 = p_client_id;
    client->client_id.size = client_id_sz;
    client->protocol_version = MQTT_VERSION_3_1_1;

    /* MQTT user name and password */
    if (init->password)
    {
        mqtt_password.utf8 = init->password;
        mqtt_password.size = strlen(init->password);
        client->password = &mqtt_password;
    }
    else
    {
        client->password = NULL;
    }

    if (init->username)
    {
        mqtt_user_name.utf8 = init->username;
        mqtt_user_name.size = strlen(init->username);
        client->user_name = &mqtt_user_name;
    }
    else
    {
        client->user_name = NULL;
    }

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
    tls_config->hostname = init->hostname;
#if defined(CONFIG_NRF_MODEM)
    tls_config->session_cache = TLS_SESSION_CACHE_DISABLED;
#endif

    return 0;
}

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

#if defined(CONFIG_NET_NATIVE)
static int load_credential(int tag, enum tls_credential_type type, uint8_t *buf, size_t buf_len)
{
    int err;
    struct pyrinas_cloud_settings_params params = {0};
    char name[64] = {0};

    snprintf(name, sizeof(name), "pyrinas/cred/%i/%i", tag, type);

    LOG_INF("Getting %s", (char *)name);

    params.len = buf_len;
    params.buf = buf;

    /* First get data from disk. */
    err = settings_load_subtree_direct(name, settings_read_callback, &params);
    if (err < 0 || !params.found)
    {
        LOG_INF("%s not found", (char *)name);
        return -EINVAL;
    }
    else
    {
        LOG_DBG("%s found", (char *)name);
    }

    LOG_HEXDUMP_DBG(params.buf, params.len, "");

    LOG_DBG("Cred is %i bytes", params.len);

    /* Then add the credential(s) */
    err = tls_credential_add(tag, type,
                             buf, params.len);
    if (err < 0)
    {
        LOG_ERR("Failed to register public certificate: %d", err);
        return err;
    }

    LOG_INF("Credential %s added! %i bytes", (char *)name, params.len);

    return 0;
}

static int tls_init(void)
{
    int err = 0;

    /* Get CA, cert and pk*/
    for (int i = TLS_CREDENTIAL_CA_CERTIFICATE; i <= TLS_CREDENTIAL_PRIVATE_KEY; i++)
    {
        err = load_credential(CONFIG_PYRINAS_CLOUD_SEC_TAG, i, cloud_credentials[i - 1], sizeof(cloud_credentials[i - 1]));
        if (err)
            LOG_WRN("Unable to load.");
    }

    return 0;
}
#endif

int pyrinas_cloud_init(struct pyrinas_cloud_config *p_config)
{
    int err;

    if (is_init)
        return -EALREADY;

    /* TODO: Determine values of all settings... */
    struct pyrinas_cloud_client_init init = {0};

#if CONFIG_SETTINGS
    struct pyrinas_cloud_settings_params params = {0};

    /* Load settings first (if not already) */
    settings_load();

#if defined(CONFIG_NET_NATIVE)
    /* Load TLS certs */
    err = tls_init();
    if (err)
    {
        LOG_ERR("Unable to load TLS certs. Err: %i", err);
        return err;
    }
#endif

    /* Set buf */
    params.len = sizeof(m_hostname);
    params.buf = m_hostname;

    err = settings_load_subtree_direct(PYRINAS_CLOUD_HOSTNAME, settings_read_callback, &params);
    if (err < 0 || !params.found)
    {
        init.hostname = CONFIG_PYRINAS_CLOUD_MQTT_BROKER_HOSTNAME;
    }
    else
    {
        LOG_DBG("hostname %s", (char *)m_hostname);
        init.hostname = m_hostname;
    }

    /* Set buf */
    params.len = sizeof(m_port);
    params.buf = m_port;

    err = settings_load_subtree_direct(PYRINAS_CLOUD_PORT, settings_read_callback, &params);
    if (err < 0 || !params.found)
    {
        init.port = CONFIG_PYRINAS_CLOUD_MQTT_BROKER_PORT;
    }
    else
    {
        LOG_DBG("port %i", atoi(m_port));
        init.port = atoi(m_port);
    }

    /* Set buf */
    params.len = sizeof(m_username);
    params.buf = m_username;

    err = settings_load_subtree_direct(PYRINAS_CLOUD_USER, settings_read_callback, &params);
    if (err < 0 || !params.found)
    {
#ifdef CONFIG_PYRINAS_CLOUD_USER_NAME
        init.username = CONFIG_PYRINAS_CLOUD_USER_NAME;
#else
        init.username = NULL;
#endif
    }
    else
    {
        LOG_DBG("user %i %s", strlen(m_username), (char *)m_username);
        init.username = m_username;
    }

    /* Set buf */
    params.len = sizeof(m_password);
    params.buf = m_password;

    err = settings_load_subtree_direct(PYRINAS_CLOUD_PASSWORD, settings_read_callback, &params);
    if (err < 0 || !params.found)
    {
#ifdef CONFIG_PYRINAS_CLOUD_PASSWORD
        init.password = CONFIG_PYRINAS_CLOUD_PASSWORD;
#else
        init.password = NULL;
#endif
    }
    else
    {
        LOG_DBG("password %i %s", strlen(m_password), (char *)m_password);
        init.password = m_password;
    }

#else
#ifdef CONFIG_PYRINAS_CLOUD_USER_NAME
    init.username = CONFIG_PYRINAS_CLOUD_USER_NAME;
#else
    init.username = NULL;
#endif

#ifdef CONFIG_PYRINAS_CLOUD_PASSWORD
    init.password = CONFIG_PYRINAS_CLOUD_PASSWORD;
#else
    init.password = NULL;
#endif

    init.port = CONFIG_PYRINAS_CLOUD_MQTT_BROKER_PORT;
    init.hostname = CONFIG_PYRINAS_CLOUD_MQTT_BROKER_HOSTNAME;
#endif

    /*Set the callback*/
    m_config = *p_config;

    LOG_DBG("Topics");

/* Set up topics */
#ifdef CONFIG_PYRINAS_CLOUD_OTA_ENABLED
    err = pyrinas_cloud_ota_init(p_config);
    if (err)
    {
        LOG_ERR("Error init OTA. Err: %i", err);
        return err;
    }

    snprintf(ota_sub_topic, sizeof(ota_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_TOPIC, p_config->client_id.len, p_config->client_id.str, 's');
    snprintf(ota_download_sub_topic, sizeof(ota_download_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_OTA_DOWNLOAD_TOPIC, p_config->client_id.len, p_config->client_id.str);
    LOG_DBG("%s %i %i", (char *)ota_sub_topic, strlen(ota_sub_topic), sizeof(ota_sub_topic));
    LOG_DBG("%s %i %i", (char *)ota_download_sub_topic, strlen(ota_download_sub_topic), sizeof(ota_download_sub_topic));
#endif

    snprintf(telemetry_pub_topic, sizeof(telemetry_pub_topic), CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_PUB_TOPIC, p_config->client_id.len, p_config->client_id.str);
    snprintf(application_sub_topic, sizeof(application_sub_topic), CONFIG_PYRINAS_CLOUD_MQTT_APPLICATION_SUB_TOPIC, p_config->client_id.len, p_config->client_id.str, 1, "+");
    LOG_DBG("%s %i %i", (char *)telemetry_pub_topic, strlen(telemetry_pub_topic), sizeof(telemetry_pub_topic));
    LOG_DBG("%s %i %i", (char *)application_sub_topic, strlen(application_sub_topic), sizeof(application_sub_topic));

    /* For debug purposes */
    LOG_INF("Connecting to: %s on port %d", init.hostname, init.port);

    /* MQTT client create */
    err = client_init(&init, &client, p_config->client_id.str, p_config->client_id.len);
    if (err != 0)
    {
        LOG_ERR("client_init %d", err);

        /* Send to calback */
        if (m_config.evt_cb)
        {
            struct pyrinas_cloud_evt evt = {.type = PYRINAS_CLOUD_EVT_ERROR,
                                            .err = err};
            m_config.evt_cb(&evt);
        }

        return err;
    }

    is_init = true;

    return 0;
}

int pyrinas_cloud_publish_evt_telemetry(pyrinas_event_t *evt)
{
    char uid[14];
    int err = 0;

    size_t data_len = 0;
    uint8_t data[CONFIG_PYRINAS_CLOUD_MQTT_TELEMETRY_SIZE] = {0};
    uint8_t topic[CONFIG_PYRINAS_CLOUD_MQTT_TOPIC_SIZE] = {0};

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
    return pyrinas_cloud_publish_raw(topic, ret, data, data_len);
}

int pyrinas_cloud_publish_evt(pyrinas_event_t *evt)
{
    char uid[14];
    char topic[CONFIG_PYRINAS_CLOUD_MQTT_TOPIC_SIZE] = {0};
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

    LOG_DBG("Sending from [%s] %s.", uid, evt->name.bytes);

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
    return pyrinas_cloud_publish_raw(topic, ret, evt->data.bytes, evt->data.size);
}

int pyrinas_cloud_publish(char *type, uint8_t *data, size_t len)
{
    int ret;
    char topic[CONFIG_PYRINAS_CLOUD_MQTT_TOPIC_SIZE] = {0};

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
    pyrinas_cloud_publish_raw(topic, ret, data, len);

    return 0;
}

static void pyrinas_rx_process(struct pyrinas_cloud_evt *p_message)
{

#if CONFIG_PYRINAS_CLOUD_OTA_ENABLED

    /* If its the OTA sub topic process */
    if (strncmp(ota_sub_topic, p_message->data.topic, p_message->data.topic_len) == 0)
    {
        LOG_DBG("Found %s. Data size: %d", (char *)ota_sub_topic, p_message->data.data_len);

        /* Set check flag */
        atomic_set(&initial_ota_check, 1);

        /* Set package*/
        pyrinas_cloud_ota_set_package(p_message->data.data, p_message->data.data_len);

        return;
    }
    else if (strncmp(ota_download_sub_topic, p_message->data.topic, p_message->data.topic_len) == 0)
    {
        int err = 0;

        LOG_DBG("ota %i bytes received", p_message->data.data_len);

        err = pyrinas_cloud_ota_set_next(p_message->data.data, p_message->data.data_len);
        if (err)
        {
            LOG_ERR("Error decoding and setting next. Err: %i", err);
        }

        return;
    }
#endif

    for (int i = 0; i < CONFIG_PYRINAS_CLOUD_APPLICATION_CALLBACK_MAX_COUNT; i++)
    {
        /* Continue if null */
        if (callbacks[i].cb == NULL)
            continue;

        LOG_DBG("%s %d", (char *)p_message->data.data, p_message->data.topic_len);

        /* Determine if this is the topic*/
        if (strncmp(callbacks[i].full_topic, p_message->data.topic, p_message->data.topic_len) == 0)
        {
            LOG_DBG("Found %s", (char *)callbacks[i].topic);

            /* Callbacks to app context */
            callbacks[i].cb(callbacks[i].topic, callbacks[i].topic_len, p_message->data.data, p_message->data.data_len);

            /* Found it,lets break */
            break;
        }
    }

    /* Send to calback */
    if (m_config.evt_cb)
    {
        m_config.evt_cb(p_message);
    }
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

    /* Set timeout for sending data */
    err = setsockopt(fds.fd, SOL_SOCKET, SO_SNDTIMEO,
                     &timeout, sizeof(timeout));
    if (err == -1)
    {
        LOG_WRN("Failed to set timeout, errno: %d", errno);
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

#ifdef CONFIG_PYRINAS_CLOUD_OTA_ENABLED
#define PYRINAS_CLOUD_THREAD_STACK_SIZE KB(8)
#else
#define PYRINAS_CLOUD_THREAD_STACK_SIZE KB(4)
#endif
K_THREAD_DEFINE(pyrinas_cloud_thread, PYRINAS_CLOUD_THREAD_STACK_SIZE,
                pyrinas_cloud_poll, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

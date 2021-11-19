/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr.h>
#include <stdio.h>
#include <nrf_modem_gnss.h>
#include <string.h>
#include <modem/at_cmd.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <modem/modem_info.h>
#include <nrf_modem.h>

// AWS-IOT code - 1 Start
#include <sys/reboot.h>
#include <date_time.h>
#include <dfu/mcuboot.h>
#include <cJSON.h>
#include <cJSON_os.h>

#include <time.h>

// UARTE
#include <sys/printk.h>
#include <drivers/uart.h>
#include <device.h>

// - MQTT Simple
#include <random/rand32.h>
#include <net/mqtt.h>
#include <net/socket.h>
#include <logging/log.h>

#include <nrf_modem.h>

#define DEBUG 3

static bool cloud_connected;
static bool mqtt_connected = false;
static uint8_t mqtt_hello[] = "{ \"type\" : 1 }";
static uint8_t mqtt_hello_size = sizeof(mqtt_hello);

LOG_MODULE_REGISTER(mqtt_simple, CONFIG_MQTT_SIMPLE_LOG_LEVEL);

/* Buffers for MQTT client. */
static uint8_t rx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t payload_buf[CONFIG_MQTT_PAYLOAD_BUFFER_SIZE];

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* File descriptor */
static struct pollfd fds;

/**@brief Function to print strings without null-termination
 */
static void data_print(uint8_t *prefix, uint8_t *data, size_t len)
{
    char buf[len + 1];

    memcpy(buf, data, len);
    buf[len] = 0;
    LOG_INF("%s%s", log_strdup(prefix), log_strdup(buf));
}

/**@brief Function to publish data on the configured topic
 */
static int data_publish(struct mqtt_client *c, enum mqtt_qos qos,
                        uint8_t *data, size_t len)
{
    struct mqtt_publish_param param;

    param.message.topic.qos = qos;
    param.message.topic.topic.utf8 = CONFIG_MQTT_PUB_TOPIC;
    param.message.topic.topic.size = strlen(CONFIG_MQTT_PUB_TOPIC);
    param.message.payload.data = data;
    param.message.payload.len = len;
    param.message_id = sys_rand32_get();
    param.dup_flag = 0;
    param.retain_flag = 0;

    if (DEBUG > 3)
    {
        data_print("Publishing: ", data, len);
        printk("to topic: %s len: %u\n",
               CONFIG_MQTT_PUB_TOPIC,
               (unsigned int)strlen(CONFIG_MQTT_PUB_TOPIC));
    }

    return mqtt_publish(c, &param);
}

/**@brief Function to subscribe to the configured topic
 */
static int subscribe(void)
{
    struct mqtt_topic subscribe_topic = {
        .topic = {
            .utf8 = CONFIG_MQTT_SUB_TOPIC,
            .size = strlen(CONFIG_MQTT_SUB_TOPIC)},
        .qos = MQTT_QOS_1_AT_LEAST_ONCE};

    const struct mqtt_subscription_list subscription_list = {
        .list = &subscribe_topic,
        .list_count = 1,
        .message_id = 1234};

    LOG_INF("Subscribing to: %s len %u", CONFIG_MQTT_SUB_TOPIC,
            (unsigned int)strlen(CONFIG_MQTT_SUB_TOPIC));

    return mqtt_subscribe(&client, &subscription_list);
}

/**@brief Function to read the published payload.
 */
static int publish_get_payload(struct mqtt_client *c, size_t length)
{
    if (length > sizeof(payload_buf))
    {
        return -EMSGSIZE;
    }

    return mqtt_readall_publish_payload(c, payload_buf, length);
}
/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const c,
                      const struct mqtt_evt *evt)
{
    int err;

    switch (evt->type)
    {
    case MQTT_EVT_CONNACK:
        if (evt->result != 0)
        {
            printk("MQTT connect failed: %d", evt->result);
            break;
        }

        printk("MQTT client connected\n");
        //cloud_connected = true;
        mqtt_connected = true;

        subscribe();
        break;

    case MQTT_EVT_DISCONNECT:
        printk("MQTT client disconnected: %d", evt->result);
        break;

    case MQTT_EVT_PUBLISH:
    {
        const struct mqtt_publish_param *p = &evt->param.publish;

        printk("MQTT PUBLISH result=%d len=%d",
               evt->result, p->message.payload.len);
        err = publish_get_payload(c, p->message.payload.len);

        if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE)
        {
            const struct mqtt_puback_param ack = {
                .message_id = p->message_id};

            /* Send acknowledgment. */
            mqtt_publish_qos1_ack(&client, &ack);
        }

        if (err >= 0)
        {
            data_print("Received: ", payload_buf,
                       p->message.payload.len);
            /* Echo back received data */
            data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE,
                         payload_buf, p->message.payload.len);
        }
        else
        {
            printk("publish_get_payload failed: %d", err);
            printk("Disconnecting MQTT client...");

            err = mqtt_disconnect(c);
            if (err)
            {
                printk("Could not disconnect: %d", err);
            }
        }
    }
    break;

    case MQTT_EVT_PUBACK:
        if (evt->result != 0)
        {
            printk("MQTT PUBACK error: %d\n", evt->result);
            break;
        }

        if (DEBUG > 3)
        {
            printk("PUBACK packet id: %u\n", evt->param.puback.message_id);
        }
        break;

    case MQTT_EVT_SUBACK:
        if (evt->result != 0)
        {
            printk("MQTT SUBACK error: %d", evt->result);
            break;
        }

        if (DEBUG > 5)
        {
            printk("SUBACK packet id: %u\n", evt->param.suback.message_id);
        }
        break;

    case MQTT_EVT_PINGRESP:
        if (evt->result != 0)
        {
            printk("MQTT PINGRESP error: %d\n", evt->result);
        }
        break;

    default:
        printk("Unhandled MQTT event type: %d\n", evt->type);
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
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM};

    err = getaddrinfo(CONFIG_MQTT_BROKER_HOSTNAME, NULL, &hints, &result);
    if (err)
    {
        printk("getaddrinfo failed: %d", err);
        return -ECHILD;
    }

    addr = result;

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
            broker4->sin_port = htons(CONFIG_MQTT_BROKER_PORT);

            inet_ntop(AF_INET, &broker4->sin_addr.s_addr,
                      ipv4_addr, sizeof(ipv4_addr));
            printk("IPv4 Address found %s\n", log_strdup(ipv4_addr));

            break;
        }
        else
        {
            printk("ai_addrlen = %u should be %u or %u",
                   (unsigned int)addr->ai_addrlen,
                   (unsigned int)sizeof(struct sockaddr_in),
                   (unsigned int)sizeof(struct sockaddr_in6));
        }

        addr = addr->ai_next;
    }

    /* Free the address. */
    freeaddrinfo(result);

    return err;
}

#if defined(CONFIG_NRF_MODEM_LIB)
#define IMEI_LEN 15
#define CGSN_RESPONSE_LENGTH 19
#define CLIENT_ID_LEN sizeof("nrf-") + IMEI_LEN
#else
#define RANDOM_LEN 10
#define CLIENT_ID_LEN sizeof(CONFIG_BOARD) + 1 + RANDOM_LEN
#endif /* defined(CONFIG_NRF_MODEM_LIB) */

/* Function to get the client id */
static const uint8_t *client_id_get(void)
{
    static uint8_t client_id[MAX(sizeof(CONFIG_MQTT_CLIENT_ID),
                                 CLIENT_ID_LEN)];

    if (strlen(CONFIG_MQTT_CLIENT_ID) > 0)
    {
        snprintf(client_id, sizeof(client_id), "%s",
                 CONFIG_MQTT_CLIENT_ID);
        goto exit;
    }

#if defined(CONFIG_NRF_MODEM_LIB)
    char imei_buf[CGSN_RESPONSE_LENGTH + 1];
    int err;

    if (!IS_ENABLED(CONFIG_AT_CMD_SYS_INIT))
    {
        err = at_cmd_init();
        if (err)
        {
            LOG_ERR("at_cmd failed to initialize, error: %d", err);
            goto exit;
        }
    }

    err = at_cmd_write("AT+CGSN", imei_buf, sizeof(imei_buf), NULL);
    if (err)
    {
        LOG_ERR("Failed to obtain IMEI, error: %d", err);
        goto exit;
    }

    imei_buf[IMEI_LEN] = '\0';

    snprintf(client_id, sizeof(client_id), "nrf-%.*s", IMEI_LEN, imei_buf);
#else
    uint32_t id = sys_rand32_get();
    snprintf(client_id, sizeof(client_id), "%s-%010u", CONFIG_BOARD, id);
#endif /* !defined(NRF_CLOUD_CLIENT_ID) */

exit:
    LOG_DBG("client_id = %s", log_strdup(client_id));

    return client_id;
}

/**@brief Initialize the MQTT client structure
 */
static int client_init(struct mqtt_client *client)
{
    int err;

    printk("client_init():\n");
    mqtt_client_init(client);

    err = broker_init();
    if (err)
    {
        printk("Failed to initialize broker connection\n");
        LOG_ERR("Failed to initialize broker connection");
        return err;
    }

    /* MQTT client configuration */
    client->broker = &broker;
    client->evt_cb = mqtt_evt_handler;
    client->client_id.utf8 = client_id_get();
    client->client_id.size = strlen(client->client_id.utf8);
    client->password = NULL;
    client->user_name = NULL;
    client->protocol_version = MQTT_VERSION_3_1_1;

    /* MQTT buffers configuration */
    client->rx_buf = rx_buffer;
    client->rx_buf_size = sizeof(rx_buffer);
    client->tx_buf = tx_buffer;
    client->tx_buf_size = sizeof(tx_buffer);

    /* MQTT transport configuration */
#if defined(CONFIG_MQTT_LIB_TLS)
    struct mqtt_sec_config *tls_cfg = &(client->transport).tls.config;
    static sec_tag_t sec_tag_list[] = {CONFIG_MQTT_TLS_SEC_TAG};

    LOG_INF("TLS enabled");
    client->transport.type = MQTT_TRANSPORT_SECURE;

    tls_cfg->peer_verify = CONFIG_MQTT_TLS_PEER_VERIFY;
    tls_cfg->cipher_count = 0;
    tls_cfg->cipher_list = NULL;
    tls_cfg->sec_tag_count = ARRAY_SIZE(sec_tag_list);
    tls_cfg->sec_tag_list = sec_tag_list;
    tls_cfg->hostname = CONFIG_MQTT_BROKER_HOSTNAME;

#if defined(CONFIG_NRF_MODEM_LIB)
    tls_cfg->session_cache = IS_ENABLED(CONFIG_MQTT_TLS_SESSION_CACHING) ? TLS_SESSION_CACHE_ENABLED : TLS_SESSION_CACHE_DISABLED;
#else
    /* TLS session caching is not supported by the Zephyr network stack */
    tls_cfg->session_cache = TLS_SESSION_CACHE_DISABLED;

#endif

#else
    client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif

    return err;
}

/**@brief Initialize the file descriptor structure used by poll.
 */
static int fds_init(struct mqtt_client *c)
{
    if (c->transport.type == MQTT_TRANSPORT_NON_SECURE)
    {
        fds.fd = c->transport.tcp.sock;
    }
    else
    {
#if defined(CONFIG_MQTT_LIB_TLS)
        fds.fd = c->transport.tls.sock;
#else
        return -ENOTSUP;
#endif
    }

    fds.events = POLLIN;

    return 0;
}

#if defined(CONFIG_DK_LIBRARY)
static void button_handler(uint32_t button_states, uint32_t has_changed)
{
    if (has_changed & button_states &
        BIT(CONFIG_BUTTON_EVENT_BTN_NUM - 1))
    {
        int ret;
        /*
		ret = data_publish(&client,
				   MQTT_QOS_1_AT_LEAST_ONCE,
				   CONFIG_BUTTON_EVENT_PUBLISH_MSG,
				   sizeof(CONFIG_BUTTON_EVENT_PUBLISH_MSG)-1);
		if (ret) {
			LOG_ERR("Publish failed: %d", ret);
		}
        */
    }
}
#endif

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static int modem_configure(void)
{
#if defined(CONFIG_LTE_LINK_CONTROL)
    /* Turn off LTE power saving features for a more responsive demo. Also,
	 * request power saving features before network registration. Some
	 * networks rejects timer updates after the device has registered to the
	 * LTE network.
	 */
    LOG_INF("Disabling PSM and eDRX");
    lte_lc_psm_req(false);
    lte_lc_edrx_req(false);

    if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT))
    {
        /* Do nothing, modem is already turned on
		 * and connected.
		 */
    }
    else
    {
#if defined(CONFIG_LWM2M_CARRIER)
        /* Wait for the LWM2M_CARRIER to configure the modem and
		 * start the connection.
		 */
        LOG_INF("Waitng for carrier registration...");
        k_sem_take(&carrier_registered, K_FOREVER);
        LOG_INF("Registered!");
#else  /* defined(CONFIG_LWM2M_CARRIER) */
        int err;

        LOG_INF("LTE Link Connecting...");
        err = lte_lc_init_and_connect();
        if (err)
        {
            LOG_INF("Failed to establish LTE connection: %d", err);
            return err;
        }
        LOG_INF("LTE Link Connected!");
#endif /* defined(CONFIG_LWM2M_CARRIER) */
    }
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */

    return 0;
}

// MQTT - END

#define AV 1
#define MAX_POWER 2000

#define IS_EMPTY_STR(X) ((1 / (sizeof(X[0]) == 1)) /*type check*/ && !(X[0]) /*content check*/)

typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;

// END

struct k_fifo uart_fifo;

static K_FIFO_DEFINE(fifo_uart_rx_data);

// Data receivced but not ready to send - queue/cache
struct rx_data_t
{
    void *fifo_reserved; // 1st word reserved for use by FIFO
    u8_t rx_string[32];  // Each message will only record a max of 32 bytes
};

struct rx_data_t *rx_data;

BUILD_ASSERT(!IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT),
             "This sample does not support LTE auto-init and connect");

#define APP_TOPICS_COUNT CONFIG_AWS_IOT_APP_SUBSCRIPTION_LIST_COUNT

#define CONFIG_CONNECTION_RETRY_TIMEOUT_SECONDS 30
#define CONFIG_PUBLICATION_INTERVAL_SECONDS 60

static struct k_work_delayable connect_work;

// AWS-IOT code - 1 End

#ifdef CONFIG_SUPL_CLIENT_LIB
#include <supl_os_client.h>
#include <supl_session.h>
#include "supl_support.h"
#endif

#define AT_XSYSTEMMODE "AT\%XSYSTEMMODE=1,0,1,0"
#define AT_ACTIVATE_GPS "AT+CFUN=31"

#define AT_CMD_SIZE(x) (sizeof(x) - 1)

#ifdef CONFIG_BOARD_NRF9160DK_NRF9160NS
#define AT_MAGPIO "AT\%XMAGPIO=1,0,0,1,1,1574,1577"
#ifdef CONFIG_GPS_SAMPLE_ANTENNA_ONBOARD
#define AT_COEX0 "AT\%XCOEX0=1,1,1565,1586"
#elif CONFIG_GPS_SAMPLE_ANTENNA_EXTERNAL
#define AT_COEX0 "AT\%XCOEX0"
#endif
#endif /* CONFIG_BOARD_NRF9160DK_NRF9160NS */

#ifdef CONFIG_BOARD_THINGY91_NRF9160NS
#define AT_MAGPIO                                          \
    "AT\%XMAGPIO=1,1,1,7,1,746,803,2,698,748,2,1710,2200," \
    "3,824,894,4,880,960,5,791,849,7,1565,1586"
#ifdef CONFIG_GPS_SAMPLE_ANTENNA_ONBOARD
#define AT_COEX0 "AT\%XCOEX0=1,1,1565,1586"
#elif CONFIG_GPS_SAMPLE_ANTENNA_EXTERNAL
#define AT_COEX0 "AT\%XCOEX0"
#endif
#endif /* CONFIG_BOARD_THINGY91_NRF9160NS */

static const char update_indicator[] = {'\\', '|', '/', '-'};
static const char *const at_commands[] = {
#if !defined(CONFIG_SUPL_CLIENT_LIB)
    AT_XSYSTEMMODE,
#endif
#if defined(CONFIG_BOARD_NRF9160DK_NRF9160NS) || \
    defined(CONFIG_BOARD_THINGY91_NRF9160NS)
    AT_MAGPIO, AT_COEX0,
#endif
    AT_ACTIVATE_GPS};

static struct nrf_modem_gnss_pvt_data_frame last_pvt;
//static struct sub_twenty last_record;
static volatile bool gnss_blocked;

#ifdef CONFIG_SUPL_CLIENT_LIB
static struct nrf_modem_gnss_agps_data_frame last_agps;
static struct k_work_q agps_work_q;
static struct k_work get_agps_data_work;

#define AGPS_WORKQ_THREAD_STACK_SIZE 2048
#define AGPS_WORKQ_THREAD_PRIORITY 5

K_THREAD_STACK_DEFINE(agps_workq_stack_area, AGPS_WORKQ_THREAD_STACK_SIZE);
#endif

// Does this need to be set to 16 as the queue needs to be ^2 ?
// https://docs.zephyrproject.org/1.14.1/reference/kernel/data_passing/message_queues.html
#define MAX_ANT_SIZE 8
#define ANT_POWER_SIZE 6

typedef struct ant_data_frame
{
    uint8_t ant_str[MAX_ANT_SIZE];
};

typedef struct
{
    //uint8_t distribution : 7;     ///< Pedal power distribution (%).
    //uint8_t differentiation : 1;  ///< Pedal differentiation: 1 -> right, 0 -> unknown.
    uint8_t pedal_power;
    uint8_t update_event_count;
    uint8_t accumulated_power[2];
    uint8_t instantaneous_power[2];
    uint8_t elapsed_time[2];
} ant_bpwr_page16_data_layout_t;

static u8_t last_ant_string[64];

// Use fifo/mq after this testing
static u8_t ant_buffer[ANT_POWER_SIZE];

int16_t lc = 0;
int8_t dump_data = 0;

struct ant_data_frame *ant_data;

K_MSGQ_DEFINE(nmea_queue, sizeof(struct nrf_modem_gnss_nmea_data_frame *), 10, 4);
K_MSGQ_DEFINE(ant_queue, sizeof(struct ant_data_frame *), 10, 4);

K_SEM_DEFINE(pvt_data_sem, 0, 1);
K_SEM_DEFINE(lte_ready, 0, 1);
K_SEM_DEFINE(ant_data_sem, 0, 1);

struct k_poll_event events[4] = {
    K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
                                    K_POLL_MODE_NOTIFY_ONLY, &pvt_data_sem,
                                    0),
    K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
                                    K_POLL_MODE_NOTIFY_ONLY, &nmea_queue,
                                    0),
    K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
                                    K_POLL_MODE_NOTIFY_ONLY, &ant_data_sem,
                                    0),
    K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
                                    K_POLL_MODE_NOTIFY_ONLY, &ant_queue,
                                    0),
};

static int json_add_obj(cJSON *parent, const char *str, cJSON *item)
{
    cJSON_AddItemToObject(parent, str, item);

    return 0;
}

static int json_add_str(cJSON *parent, const char *str, const char *item)
{
    cJSON *json_str;

    json_str = cJSON_CreateString(item);
    if (json_str == NULL)
    {
        return -ENOMEM;
    }

    return json_add_obj(parent, str, json_str);
}

static int json_add_number(cJSON *parent, const char *str, double item)
{
    cJSON *json_num;

    json_num = cJSON_CreateNumber(item);
    if (json_num == NULL)
    {
        printk("json_add_number: Failed %s\n", str);
        return -ENOMEM;
    }

    return json_add_obj(parent, str, json_num);
}

static void connect_work_fn(struct k_work *work)
{

    printk("connect_work_fn()\n");
    if (cloud_connected)
    {
        printk("cloud_connected()\n");
        return;
    }

    printk("Next connection retry in %d seconds\n",
           CONFIG_CONNECTION_RETRY_TIMEOUT_SECONDS);

    k_work_schedule(&connect_work,
                    K_SECONDS(CONFIG_CONNECTION_RETRY_TIMEOUT_SECONDS));
}

static void work_init(void)
{
    printk("work_init()\n");
    //k_work_init_delayable(&shadow_update_work, shadow_update_work_fn);
    k_work_init_delayable(&connect_work, connect_work_fn);
    //k_work_init(&shadow_update_version_work, shadow_update_version_work_fn);
}

void nrf_modem_recoverable_error_handler(uint32_t error)
{
    printk("Modem library recoverable error: %u\n", error);
}

static void show_battery_voltage()
{
    char info_str[MODEM_INFO_MAX_RESPONSE_SIZE + 1];
    int err;

    err = modem_info_string_get(MODEM_INFO_BATTERY, info_str, sizeof(info_str));
    if (err < 0)
    {
        printk("MODEM_INFO_BATTERY, error: %d\n", err);
        return;
    }

    printk("MODEM_INFO_BATTERY: %s\n", info_str);
}

static void show_modem_ip()
{
    char info_str[MODEM_INFO_MAX_RESPONSE_SIZE + 1];
    int err;

    err = modem_info_string_get(MODEM_INFO_IP_ADDRESS, info_str, sizeof(info_str));
    if (err < 0)
    {
        printk("MODEM_INFO_IP_ADDRESS, error: %d\n", err);
        return;
    }

    printk("MODEM_INFO_IP_ADDRESS: %s\n", info_str);
}

static void show_modem_date_time()
{
    char info_str[MODEM_INFO_MAX_RESPONSE_SIZE + 1];
    int err;

    err = modem_info_string_get(MODEM_INFO_DATE_TIME, info_str, sizeof(info_str));
    if (err < 0)
    {
        printk("MODEM_INFO_DATE_TIME, error: %d\n", err);
        return;
    }

    printk("MODEM_INFO_DATE_TIME: %s\n", info_str);
}

static void show_modem_gps()
{
    char info_str[MODEM_INFO_MAX_RESPONSE_SIZE + 1];
    int err;

    err = modem_info_string_get(MODEM_INFO_GPS_MODE, info_str, sizeof(info_str));
    if (err < 0)
    {
        printk("MODEM_INFO_GPS_MODE, error: %d\n", err);
        return;
    }

    printk("MODEM_INFO_GPS_MODE: %s\n", info_str);
}

static int setup_modem(void)
{
    //display_state = LEDS_LTE_CONNECTING;
    int err;

    err = modem_info_init();
    if (err)
    {
        printk("modem_info_params_init, error: %d\n", err);
        return -1;
    }

    show_battery_voltage();
    show_modem_ip();
    show_modem_gps();
    show_modem_date_time();

    if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT))
    {
        /* Do nothing, modem is already turned on and connected. */
    }
    else
    {
        //int err;

        printk("lte_lc_init()\n");
        err = lte_lc_init();
        if (err)
        {
            printk("lte_lc_init, error: %d\n", err);
            return -1;
        }

        printk("lte_lc_psm_req()\n");
        err = lte_lc_psm_req(true);
        if (err)
        {
            printk("lte_lc_psm_req, error: %d\n", err);
            return -1;
        }
        printk("Establishing LTE link (this may take some time) ...\n");
        err = lte_lc_connect();
        __ASSERT(err == 0, "LTE link could not be established.");

        //display_state = LEDS_LTE_CONNECTED;

        //struct lte_lc_conn_eval_params params = { 0 };

        //err = lte_lc_conn_eval_params_get(&params);
        //if (err) {
        //	printk("lte_lc_conn_eval_params_get, error: %d\n", err);
        //	return -1;
        //}
    }
    printk("Leaving setup_modem()\n");

    show_battery_voltage();
    show_modem_ip();
    show_modem_gps();

    return 0;
    for (int i = 0; i < ARRAY_SIZE(at_commands); i++)
    {
        if (at_cmd_write(at_commands[i], NULL, 0, NULL) != 0)
        {
            return -1;
        }
    }

    return 0;
}

#ifdef CONFIG_SUPL_CLIENT_LIB
BUILD_ASSERT(IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_GPS) ||
                 IS_ENABLED(CONFIG_LTE_NETWORK_MODE_NBIOT_GPS) ||
                 IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS),
             "To use SUPL and GPS, CONFIG_LTE_NETWORK_MODE_LTE_M_GPS, "
             "CONFIG_LTE_NETWORK_MODE_NBIOT_GPS or "
             "CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS must be enabled");

static void lte_handler(const struct lte_lc_evt *const evt)
{
    switch (evt->type)
    {
    case LTE_LC_EVT_NW_REG_STATUS:
        if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
            (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING))
        {
            printk("Connected to LTE network\n");
            k_sem_give(&lte_ready);
        }
        break;
    default:
        break;
    }
}

static int activate_lte(bool activate)
{
    int err;

    if (activate)
    {
        err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
        if (err)
        {
            printk("Failed to activate LTE, error: %d\n", err);
            return -1;
        }

        printk("LTE activated\n");
        k_sem_take(&lte_ready, K_FOREVER);
    }
    else
    {
        err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE);
        if (err)
        {
            printk("Failed to deactivate LTE, error: %d\n", err);
            return -1;
        }

        printk("LTE deactivated\n");
    }

    return 0;
}

static void get_agps_data(struct k_work *item)
{
    ARG_UNUSED(item);

    printk("\033[1;1H");
    printk("\033[2J");
    printk("New A-GPS data requested, contacting SUPL server, flags %d\n",
           last_agps.data_flags);

    activate_lte(true);

    if (open_supl_socket() == 0)
    {
        printk("Starting SUPL session\n");
        supl_session(&last_agps);
        printk("Done\n");
        close_supl_socket();
    }
    activate_lte(false);
}

static int inject_agps_type(void *agps, size_t agps_size, uint16_t type,
                            void *user_data)
{
    ARG_UNUSED(user_data);

    int retval = nrf_modem_gnss_agps_write(agps, agps_size, type);

    if (retval != 0)
    {
        printk("Failed to write A-GPS data, type: %d (errno: %d)\n",
               type, errno);
        return -1;
    }

    printk("Injected A-GPS data, type: %d, size: %d\n", type, agps_size);

    return 0;
}
#endif

static void gnss_event_handler(int event)
{

    int retval;
    struct nrf_modem_gnss_nmea_data_frame *nmea_data;

    switch (event)
    {
    case NRF_MODEM_GNSS_EVT_PVT:
        //printk("Received an gnss_event NRF_MODEM_GNSS_EVT_PVT\n");
        retval = nrf_modem_gnss_read(&last_pvt, sizeof(last_pvt),
                                     NRF_MODEM_GNSS_DATA_PVT);
        if (retval == 0)
        {
            k_sem_give(&pvt_data_sem);
        }
        break;

    case NRF_MODEM_GNSS_EVT_NMEA:
        //printk("Received an gnss_event NRF_MODEM_GNSS_EVT_NMEA\n");
        nmea_data =
            k_malloc(sizeof(struct nrf_modem_gnss_nmea_data_frame));
        if (nmea_data == NULL)
        {
            printk("Failed to allocate memory for NMEA\n");
            break;
        }

        retval = nrf_modem_gnss_read(
            nmea_data,
            sizeof(struct nrf_modem_gnss_nmea_data_frame),
            NRF_MODEM_GNSS_DATA_NMEA);
        if (retval == 0)
        {
            retval = k_msgq_put(&nmea_queue, &nmea_data, K_NO_WAIT);
        }

        if (retval != 0)
        {
            k_free(nmea_data);
        }
        break;

    case NRF_MODEM_GNSS_EVT_AGPS_REQ:
        //printk("Received an gnss_event NRF_MODEM_GNSS_EVT_AGPS_REQ\n");
#ifdef CONFIG_SUPL_CLIENT_LIB
        retval = nrf_modem_gnss_read(&last_agps, sizeof(last_agps),
                                     NRF_MODEM_GNSS_DATA_AGPS_REQ);
        if (retval == 0)
        {
            k_work_submit_to_queue(&agps_work_q,
                                   &get_agps_data_work);
        }
#endif
        break;

    case NRF_MODEM_GNSS_EVT_BLOCKED:
        //printk("Received an gnss_event NRF_MODEM_GNSS_EVT_BLOCKED\n");
        gnss_blocked = true;
        break;

    case NRF_MODEM_GNSS_EVT_UNBLOCKED:
        //printk("Received an gnss_event NRF_MODEM_GNSS_EVT_UNBLOCKED\n");
        gnss_blocked = false;
        break;

    default:
        //printk("Received an gnss_event default\n");
        break;
    }
}

static int init_app(void)
{
#ifdef CONFIG_SUPL_CLIENT_LIB
    if (lte_lc_init())
    {
        printk("Failed to initialize LTE link controller\n");
        return -1;
    }

    lte_lc_register_handler(lte_handler);
#endif /* CONFIG_SUPL_CLIENT_LIB */

    if (setup_modem() != 0)
    {
        printk("Failed to initialize modem\n");
        return -1;
    }

    /* Initialize and configure GNSS */
    if (nrf_modem_gnss_init() != 0)
    {
        printk("Failed to initialize GNSS interface\n");
        return -1;
    }

    if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0)
    {
        printk("Failed to set GNSS event handler\n");
        return -1;
    }

    if (nrf_modem_gnss_nmea_mask_set(NRF_MODEM_GNSS_NMEA_RMC_MASK |
                                     NRF_MODEM_GNSS_NMEA_GGA_MASK |
                                     NRF_MODEM_GNSS_NMEA_GLL_MASK |
                                     NRF_MODEM_GNSS_NMEA_GSA_MASK |
                                     NRF_MODEM_GNSS_NMEA_GSV_MASK) != 0)
    {
        printk("Failed to set GNSS NMEA mask\n");
        return -1;
    }

    if (nrf_modem_gnss_fix_retry_set(0) != 0)
    {
        printk("Failed to set GNSS fix retry\n");
        return -1;
    }

    if (nrf_modem_gnss_fix_interval_set(1) != 0)
    {
        printk("Failed to set GNSS fix interval\n");
        return -1;
    }

    if (nrf_modem_gnss_start() != 0)
    {
        printk("Failed to start GNSS\n");
        return -1;
    }

#ifdef CONFIG_SUPL_CLIENT_LIB
    static struct supl_api supl_api = {.read = supl_read,
                                       .write = supl_write,
                                       .handler = inject_agps_type,
                                       .logger = supl_logger,
                                       .counter_ms = k_uptime_get};

    k_work_queue_start(&agps_work_q, agps_workq_stack_area,
                       K_THREAD_STACK_SIZEOF(agps_workq_stack_area),
                       AGPS_WORKQ_THREAD_PRIORITY, NULL);

    k_work_init(&get_agps_data_work, get_agps_data);

    if (supl_init(&supl_api) != 0)
    {
        printk("Failed to initialize SUPL library\n");
        return -1;
    }
#endif

    return 0;
}

static void
print_satellite_stats(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
    uint8_t tracked = 0;
    uint8_t in_fix = 0;
    uint8_t unhealthy = 0;

    for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; ++i)
    {
        if (pvt_data->sv[i].sv > 0)
        {
            tracked++;

            if (pvt_data->sv[i].flags &
                NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX)
            {
                in_fix++;
            }

            if (pvt_data->sv[i].flags &
                NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY)
            {
                unhealthy++;
            }
        }
    }

    printk("Tracking: %d Using: %d Unhealthy: %d\n", tracked, in_fix,
           unhealthy);
}

static void print_fix_data(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
    printk("Latitude:   %.06f\n", pvt_data->latitude);
    printk("Longitude:  %.06f\n", pvt_data->longitude);
    printk("Altitude:   %.01f m\n", pvt_data->altitude);
    printk("Accuracy:   %.01f m\n", pvt_data->accuracy);
    printk("Speed:      %.01f m/s\n", pvt_data->speed);
    printk("Heading:    %.01f deg\n", pvt_data->heading);
    printk("Date:       %02u-%02u-%02u\n", pvt_data->datetime.year,
           pvt_data->datetime.month, pvt_data->datetime.day);
    printk("Time (UTC): %02u:%02u:%02u\n", pvt_data->datetime.hour,
           pvt_data->datetime.minute, pvt_data->datetime.seconds);
}

static bool agps_data_download_ongoing(void)
{
#ifdef CONFIG_SUPL_CLIENT_LIB
    return k_work_is_pending(&get_agps_data_work);
#else
    return false;
#endif
}

/**@brief Function for encoding a uint16 value.
 *
 * @param[in]   value            Value to be encoded.
 * @param[out]  p_encoded_data   Buffer where the encoded data is to be written.
 *
 * @return      Number of bytes written.
 */
static __INLINE uint8_t uint16_encode(uint16_t value, uint8_t *p_encoded_data)
{
    p_encoded_data[0] = (uint8_t)((value & 0x00FF) >> 0);
    p_encoded_data[1] = (uint8_t)((value & 0xFF00) >> 8);
    return sizeof(uint16_t);
}
/**@brief Function for encoding a uint32 value.
 *
 * @param[in]   value            Value to be encoded.
 * @param[out]  p_encoded_data   Buffer where the encoded data is to be written.
 *
 * @return      Number of bytes written.
 */
static __INLINE uint8_t uint32_encode(uint32_t value, uint8_t *p_encoded_data)
{
    p_encoded_data[0] = (uint8_t)((value & 0x000000FF) >> 0);
    p_encoded_data[1] = (uint8_t)((value & 0x0000FF00) >> 8);
    p_encoded_data[2] = (uint8_t)((value & 0x00FF0000) >> 16);
    p_encoded_data[3] = (uint8_t)((value & 0xFF000000) >> 24);
    return sizeof(uint32_t);
}

static __INLINE uint16_t uint16_decode(const uint8_t *p_encoded_data)
{
    return ((((uint16_t)((uint8_t *)p_encoded_data)[0])) |
            (((uint16_t)((uint8_t *)p_encoded_data)[1]) << 8));
}

/**@brief Function for decoding a uint32 value.
 *
 * @param[in]   p_encoded_data   Buffer where the encoded data is stored.
 *
 * @return      Decoded value.
 */
static __INLINE uint32_t uint32_decode(const uint8_t *p_encoded_data)
{
    return ((((uint32_t)((uint8_t *)p_encoded_data)[0]) << 0) |
            (((uint32_t)((uint8_t *)p_encoded_data)[1]) << 8) |
            (((uint32_t)((uint8_t *)p_encoded_data)[2]) << 16) |
            (((uint32_t)((uint8_t *)p_encoded_data)[3]) << 24));
}

// Called from semaphore setting
static void f_dump_data(struct ant_data_frame *ant_data)
{
    int err;
    int ret;
    uint8_t send_str[16];
    char *message;
    uint32_t elapsed_time = (k_uptime_get_32() / 1000);

    //printk("f_dump_data:\n");
    // Raw Dump for testing
    if (DEBUG > 2)
    {
        for (uint8_t i = 0; i < sizeof(ant_data->ant_str); i++)
        {
            printk("%02X ", ant_data->ant_str[i]);
        }
        printk("\n");
    }

    ant_bpwr_page16_data_layout_t const *p_incoming_data =
        (ant_bpwr_page16_data_layout_t *)ant_data;

    printk("update_event_count:  %u\n", p_incoming_data->update_event_count);
    //printk("pedal_power:         %u\n", p_incoming_data->pedal_power);

    if (DEBUG > 3)
    {
        printk("accumulated_power:   %u\n", uint16_decode(p_incoming_data->accumulated_power));
        printk("instantaneous_power: %u\n", uint16_decode(p_incoming_data->instantaneous_power));
        printk("elapsed_time:        %u\n", uint16_decode(p_incoming_data->elapsed_time));
    }

    // This is when things are corrupted
    if (uint16_decode(p_incoming_data->instantaneous_power) > MAX_POWER)
    {
        printk("Too Strong:\n");
        return;
    };

    //cJSON *reported_obj = cJSON_CreateObject();
    cJSON *root_obj = cJSON_CreateObject();

    err += json_add_number(root_obj, "e", (double)p_incoming_data->update_event_count);
    err += json_add_number(root_obj, "a", (double)uint16_decode(p_incoming_data->accumulated_power));
    err += json_add_number(root_obj, "i", (double)uint16_decode(p_incoming_data->instantaneous_power));
    err += json_add_number(root_obj, "t", (double)uint16_decode(p_incoming_data->elapsed_time));

    //err += json_add_obj(root_obj, "p", reported_obj);

    message = cJSON_Print(root_obj);

    if (message == NULL)
    {
        printk("cJSON_Print, error: returned NULL\n");
        err = -ENOMEM;
    }
    else
    {
        // Need to Discover this from a different message at startup
        char send_topic[] = "s20/60412";
        /* Removed for testing
        struct aws_iot_data gps_data = {
            .qos = MQTT_QOS_0_AT_MOST_ONCE,
            .topic.type = 0,
            .topic.str = send_topic,
            .topic.len = strlen(send_topic),
            .ptr = message,
            .len = strlen(message)};
*/
        ret = data_publish(&client,
                           MQTT_QOS_1_AT_LEAST_ONCE,
                           message,
                           strlen(message));
        if (ret)
        {
            printk("Publish failed: %d\n", ret);
        }
        if (DEBUG > 3)
        {
            printk("Publishing: (%s) to (%s)\n", message, send_topic);
        }
    }
    cJSON_FreeString(message);
    cJSON_Delete(root_obj);
}

void uart_cb(struct device *x)
{

    if (!uart_irq_update(x))
    {
        printk("should always be 1\n");
        return;
    };
    int data_length = 0;

    uint8_t uart_buf;
    uint32_t elapsed_time = (k_uptime_get_32() / 1000);
    uint8_t et[2];
    uint16_encode(elapsed_time, et);

    if (uart_irq_rx_ready(x))
    {
        // Apparently uart_fifo_read only reads 1 character at a time anyway ?
        data_length = uart_fifo_read(x, &uart_buf, 1);
        printk("%02X ", uart_buf);

        if (lc < sizeof(ant_buffer))
        {
            ant_buffer[lc] = uart_buf;
        }

        // Page 16 is 6 bytes - hardcoding for now
        if (lc++ >= 5)
        {
            int retval;
            struct ant_data_frame *ant_data;

            ant_data = k_malloc(sizeof(struct ant_data_frame));

            if (ant_data == NULL)
            {
                printk("Failed to allocate memory for ANT\n");
                return;
            }
            memset(ant_data, 0x00, sizeof(struct ant_data_frame));

            // Not sure if strlcpy is correct - not all the data was the same...
            if (DEBUG > 3)
            {
                printk("\nCopied %u bytes to ant_data\n", sizeof(ant_buffer));
            }
            strlcpy(ant_data->ant_str, ant_buffer, sizeof(ant_buffer));
            ant_data->ant_str[6] = et[0];
            ant_data->ant_str[7] = et[1];

            k_sem_give(&ant_data_sem);
            retval = k_msgq_put(&ant_queue, &ant_data, K_NO_WAIT);

            if (retval != 0)
            {
                printk("Copied %u bytes to ant_data\n", sizeof(ant_buffer));
                k_free(ant_data);
            }
            //k_free(ant_data);

            memset(ant_buffer, 0x00, sizeof(ant_buffer));

            lc = 0;
            printk("\n");
        }
    }
}

int main(void)
{
    int err;

    uint8_t cnt = 0;
    uint64_t fix_timestamp = 0;
    struct nrf_modem_gnss_nmea_data_frame *nmea_data;
    uint32_t connect_attempt = 0;

    // Setup the UART_1 first, so we can capture all data on UART even if the
    // Modem is not configured.
    struct device *uart = device_get_binding("UART_1");

    uart_irq_callback_set(uart, uart_cb);
    uart_irq_rx_enable(uart);
    // End

    if (init_app() != 0)
    {
        printk("init_app: failed\n");
        return -1;
    }

    printk("Getting GNSS data...\n");

    printk("client_init:\n");
    err = client_init(&client);
    if (err != 0)
    {
        LOG_ERR("Error: client_init: %d", err);
        printk("Error client_init: %d\n", err);
        return;
    }

/* **** 
TODO:
Need to look at the order and logic, to make sure we can still get the 
UART data even if we dont have a connection
****/
do_connect:
    if (connect_attempt++ > 0)
    {
        printk("Reconnecting in %d seconds...",
               CONFIG_MQTT_RECONNECT_DELAY_S);
        k_sleep(K_SECONDS(CONFIG_MQTT_RECONNECT_DELAY_S));
    }

    if (DEBUG > 4)
        printk("mqtt_connect:\n");

    err = mqtt_connect(&client);
    if (err != 0)
    {
        LOG_ERR("mqtt_connect: failed %d", err);
        printk("mqtt_connect %d\n", err);
        goto do_connect;
    }

    if (DEBUG > 5)
        printk("fds_init:\n");
    err = fds_init(&client);
    if (err != 0)
    {
        LOG_ERR("fds_init: %d", err);
        printk("fds_init: failed %d\n", err);
        return;
    }

    if (DEBUG > 5)
        printk("Setup the work code\n");
    cJSON_Init();

    /*
  
    work_init();

    k_work_schedule(&connect_work, K_NO_WAIT);
    */

    // Random sleep for 5 seconds to see if the MQTT connects - HACK !!
    //k_sleep(K_MSEC(5000));

    while (1)
    {
        int ret;

        cnt++;

        if (mqtt_connected)
        {
            // Ant QUEUE
            (void)k_poll(events, 4, K_FOREVER);
/* 28-Oct
            if (last_pvt.flags &
                NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)
            {

                //cJSON *reported_obj = cJSON_CreateObject();
                //cJSON *root_obj = cJSON_CreateObject();

                fix_timestamp = k_uptime_get();
                print_fix_data(&last_pvt);

                struct tm t;
                time_t t_of_day;
                double dt;

                t.tm_year = last_pvt.datetime.year - 1900; // Year - 1900
                t.tm_mon = last_pvt.datetime.month;        // Month, where 0 = jan
                t.tm_mday = last_pvt.datetime.day;         // Day of the month
                t.tm_hour = last_pvt.datetime.hour;
                t.tm_min = last_pvt.datetime.minute;
                t.tm_sec = last_pvt.datetime.seconds;
                t.tm_isdst = -1; // Is DST on? 1 = yes, 0 = no, -1 = unknown
                t_of_day = mktime(&t);

                dt = (double)t_of_day;
                */
/*
                err += json_add_number(reported_obj, "et", dt);
                err += json_add_number(reported_obj, "la", last_pvt.latitude);
                err += json_add_number(reported_obj, "lo", last_pvt.longitude);
                //err += json_add_number(reported_obj, "al", last_pvt.altitude);
                err += json_add_number(reported_obj, "sp", last_pvt.speed);

                err += json_add_obj(root_obj, "gps", reported_obj);

                message = cJSON_Print(root_obj);

                if (message == NULL)
                {
                    printk("cJSON_Print, error: returned NULL\n");
                    err = -ENOMEM;
                }
                else
                {
                    /* Removed for testing
                char send_topic[] = "sensor/sub20";
                struct aws_iot_data gps_data = {
                    .qos = MQTT_QOS_0_AT_MOST_ONCE,
                    .topic.type = 0,
                    .topic.str = send_topic,
                    .topic.len = strlen(send_topic),
                    .ptr = message,
                    .len = strlen(message)};

                printk("Publishing: (%s) to (%s)\n", message, send_topic);


                }
*/
                //cJSON_FreeString(message);
                //cJSON_Delete(root_obj);
/* 20-Oct
                show_modem_gps();
                print_satellite_stats(&last_pvt);
            }
            */

            if (events[2].state == K_POLL_STATE_SEM_AVAILABLE &&
                k_sem_take(events[2].sem, K_NO_WAIT) == 0)
            {
                printk("Semaphore 2 ready\n");
            }

            // Ant Data
            if (events[3].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE)
            {
                while (k_msgq_get(events[3].msgq, &ant_data, K_NO_WAIT) == 0)
                {
                    f_dump_data(ant_data);
                    k_free(ant_data);
                }
            }
            // Clear all

            events[0].state = K_POLL_STATE_NOT_READY;
            events[1].state = K_POLL_STATE_NOT_READY;
            events[2].state = K_POLL_STATE_NOT_READY;
            events[3].state = K_POLL_STATE_NOT_READY;
        }

        if (DEBUG > 8)
            printk("Out of mqtt_connected condition:\n");

        //err = poll(&fds, 1, mqtt_keepalive_time_left(&client));
        err = poll(&fds, 1, 1500);
        if (err < 0)
        {
            printf("poll: %d\n", errno);
            break;
        }

        if (DEBUG > 8)
            printk("mqtt_live:\n");
        err = mqtt_live(&client);
        if ((err != 0) && (err != -EAGAIN))
        {
            printf("ERROR: mqtt_live: %d\n", err);
            break;
        }

        if (DEBUG > 8)
            printk("pollin:\n");
        if ((fds.revents & POLLIN) == POLLIN)
        {
            err = mqtt_input(&client);
            if (err != 0)
            {
                printf("mqtt_input: %d\n", err);
                break;
            }
        }

        if (DEBUG > 8)
            printk("pollerr:\n");
        if ((fds.revents & POLLERR) == POLLERR)
        {
            printf("POLLERR\n");
            break;
        }

        if (DEBUG > 8)
            printk("pollnval:\n");
        if ((fds.revents & POLLNVAL) == POLLNVAL)
        {
            printf("POLLNVAL\n");
            break;
        }
        if (DEBUG > 8)
            printk("End of While - back to top\n");
    }

    for (;;)
    {
        // Need to be connected to cloud before we start pulling data
        // off the queue - might need to make the queue bigger ?
        if (!cloud_connected)
        {
            printk("Not connected yet !!!\n");
            k_sleep(K_MSEC(2000));
            continue;
        }

        (void)k_poll(events, 4, K_FOREVER);

        if (events[2].state == K_POLL_STATE_SEM_AVAILABLE &&
            k_sem_take(events[2].sem, K_NO_WAIT) == 0)
        {
            printk("Semaphore 2 ready\n");
        }

        if (events[3].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE &&
            k_msgq_get(events[3].msgq, &ant_data, K_NO_WAIT) == 0)
        {
            f_dump_data(ant_data);
            k_free(ant_data);
        }

        events[0].state = K_POLL_STATE_NOT_READY;
        events[1].state = K_POLL_STATE_NOT_READY;
        events[2].state = K_POLL_STATE_NOT_READY;
        events[3].state = K_POLL_STATE_NOT_READY;
    }

    for (;;)
    {
        if (!cloud_connected)
        {
            printk("Not connected yet !!!\n");
            k_sleep(K_MSEC(2000));
            continue;
        }

        (void)k_poll(events, 3, K_FOREVER);

        if (events[0].state == K_POLL_STATE_SEM_AVAILABLE &&
            k_sem_take(events[0].sem, K_NO_WAIT) == 0)
        {
            /* New PVT data available */

            if (!IS_ENABLED(CONFIG_GPS_SAMPLE_NMEA_ONLY) &&
                !agps_data_download_ongoing())
            {
                print_satellite_stats(&last_pvt);

                if (gnss_blocked)
                {
                    printk("GNSS operation blocked by LTE\n");
                }

                if (last_pvt.flags &
                    NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)
                {
                    fix_timestamp = k_uptime_get();
                    print_fix_data(&last_pvt);
                }
            }
        }
        else if (events[2].state == K_POLL_STATE_SEM_AVAILABLE &&
                 k_sem_take(events[2].sem, K_NO_WAIT) == 0)
        {
            printk("Semaphore 2 ready\n");
        }
        if (events[3].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE &&
            k_msgq_get(events[3].msgq, &ant_data, K_NO_WAIT) == 0)
        {
            printk("%s", ant_data->ant_str);
            k_free(ant_data);
        }
        events[0].state = K_POLL_STATE_NOT_READY;
        events[1].state = K_POLL_STATE_NOT_READY;
        events[2].state = K_POLL_STATE_NOT_READY;
        events[3].state = K_POLL_STATE_NOT_READY;
    }

    for (;;)
    {
        char *message;

        if (!cloud_connected)
        {
            printk("Not connected yet !!!\n");
            k_sleep(K_MSEC(2000));
            continue;
        }

        show_modem_gps();
        print_satellite_stats(&last_pvt);

        if (last_pvt.flags &
            NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)
        {

            cJSON *reported_obj = cJSON_CreateObject();
            cJSON *root_obj = cJSON_CreateObject();

            fix_timestamp = k_uptime_get();
            print_fix_data(&last_pvt);

            struct tm t;
            time_t t_of_day;
            double dt;

            t.tm_year = last_pvt.datetime.year - 1900; // Year - 1900
            t.tm_mon = last_pvt.datetime.month;        // Month, where 0 = jan
            t.tm_mday = last_pvt.datetime.day;         // Day of the month
            t.tm_hour = last_pvt.datetime.hour;
            t.tm_min = last_pvt.datetime.minute;
            t.tm_sec = last_pvt.datetime.seconds;
            t.tm_isdst = -1; // Is DST on? 1 = yes, 0 = no, -1 = unknown
            t_of_day = mktime(&t);

            dt = (double)t_of_day;

            err += json_add_number(reported_obj, "et", dt);
            err += json_add_number(reported_obj, "la", last_pvt.latitude);
            err += json_add_number(reported_obj, "lo", last_pvt.longitude);
            //err += json_add_number(reported_obj, "al", last_pvt.altitude);
            err += json_add_number(reported_obj, "sp", last_pvt.speed);

            err += json_add_obj(root_obj, "gps", reported_obj);

            message = cJSON_Print(root_obj);

            if (message == NULL)
            {
                printk("cJSON_Print, error: returned NULL\n");
                err = -ENOMEM;
            }
            else
            {
                /* Removed for testing
                char send_topic[] = "sensor/sub20";
                struct aws_iot_data gps_data = {
                    .qos = MQTT_QOS_0_AT_MOST_ONCE,
                    .topic.type = 0,
                    .topic.str = send_topic,
                    .topic.len = strlen(send_topic),
                    .ptr = message,
                    .len = strlen(message)};

                printk("Publishing: (%s) to (%s)\n", message, send_topic);
*/
            }
            cJSON_FreeString(message);
            cJSON_Delete(root_obj);

            show_modem_gps();
            print_satellite_stats(&last_pvt);
        }
        // Make sure we have a cloud connection before sending data
        // Put this in a function so we can loop over all the strings in the array/list
        if (cloud_connected && dump_data)
        {

            printk("Cloud Connected: send random data\n");
            //f_dump_data();
            dump_data = 0;
        }
        k_sleep(K_MSEC(5000));
    }

    for (;;)
    {
        (void)k_poll(events, 2, K_FOREVER);

        if (events[0].state == K_POLL_STATE_SEM_AVAILABLE &&
            k_sem_take(events[0].sem, K_NO_WAIT) == 0)
        {
            /* New PVT data available */

            if (!IS_ENABLED(CONFIG_GPS_SAMPLE_NMEA_ONLY) &&
                !agps_data_download_ongoing())
            {
                printk("\033[1;1H");
                printk("\033[2J");
                print_satellite_stats(&last_pvt);

                if (gnss_blocked)
                {
                    printk("GNSS operation blocked by LTE\n");
                }
                printk("---------------------------------\n");

                if (last_pvt.flags &
                    NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)
                {
                    fix_timestamp = k_uptime_get();
                    print_fix_data(&last_pvt);
                }
                else
                {
                    printk("Seconds since last fix: %lld\n",
                           (k_uptime_get() -
                            fix_timestamp) /
                               1000);
                    cnt++;
                    printk("Searching [%c]\n",
                           update_indicator[cnt % 4]);
                }

                printk("\nNMEA strings:\n\n");
            }
        }
        if (events[1].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE &&
            k_msgq_get(events[1].msgq, &nmea_data, K_NO_WAIT) == 0)
        {
            /* New NMEA data available */

            if (!agps_data_download_ongoing())
            {
                printk("%s", nmea_data->nmea_str);
            }
            k_free(nmea_data);
        }

        events[0].state = K_POLL_STATE_NOT_READY;
        events[1].state = K_POLL_STATE_NOT_READY;
    }

    return 0;
}

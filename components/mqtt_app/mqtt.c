#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"

#include "mqtt_client.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "coordinates_data.pb.h"
#include "coordinates_encoder.h"

#include "mqtt_app.h"

#include <stdint.h>

static const char *TAG = "mqtt_app";

static payload_app_config_t s_config = {0};
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static EventGroupHandle_t s_mqtt_event_group = 0;
static char s_ping_pub_topic[72];
static char s_ping_sub_topic[72];
static char s_data_pub_topic[72];
static char s_data_sub_topic[72];
static uint32_t s_last_published_seq = 0;
static uint32_t s_ping_sequence = 1;

#define MQTT_EVENT_CONNECTED_BIT BIT0
#define MQTT_EVENT_DATA_READY_BIT BIT1

// Tune these sizes to control MQTT payload lengths.
#define MQTT_DATA_PAYLOAD_BUFFER_SIZE 256
// 2048 4096 5120 10240
#define MQTT_PING_PACKET_SIZE 4096
static uint8_t s_mqtt_ping_payload[MQTT_PING_PACKET_SIZE];

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

static bool decode_coordinates_payload(const uint8_t *payload, size_t payload_len, coordinates_data *out)
{
    if (payload == NULL || out == NULL)
    {
        return false;
    }

    *out = (coordinates_data)coordinates_data_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_len);
    return pb_decode(&stream, coordinates_data_fields, out);
}

static void fill_random_bytes(uint8_t *buffer, size_t len)
{
    size_t offset = 0;
    while (offset < len)
    {
        uint32_t rnd = esp_random();
        size_t chunk = (len - offset) < sizeof(rnd) ? (len - offset) : sizeof(rnd);
        memcpy(buffer + offset, &rnd, chunk);
        offset += chunk;
    }
}

static bool extract_sent_ms_from_payload(const char *data, int data_len, unsigned long long *sent_ms)
{
    const char marker[] = "sent_ms=";
    const size_t marker_len = sizeof(marker) - 1;

    if (data == NULL || data_len <= 0 || sent_ms == NULL)
    {
        return false;
    }

    for (int i = 0; i <= data_len - (int)marker_len; ++i)
    {
        if (memcmp(data + i, marker, marker_len) == 0)
        {
            unsigned long long value = 0;
            bool has_digit = false;

            for (int j = i + (int)marker_len; j < data_len; ++j)
            {
                unsigned char c = (unsigned char)data[j];
                if (!isdigit(c))
                {
                    break;
                }

                has_digit = true;
                value = value * 10ULL + (unsigned long long)(c - '0');
            }

            if (has_digit)
            {
                *sent_ms = value;
                return true;
            }
        }
    }

    return false;
}

static bool encode_device_data_and_publish(uint32_t current_seq)
{
    uint8_t payload[MQTT_DATA_PAYLOAD_BUFFER_SIZE];
    size_t encoded_len = 0;
    float x = s_config.x ? *s_config.x : 0.0f;
    float y = s_config.y ? *s_config.y : 0.0f;

    if (!encode_coordinates(payload, sizeof(payload), &encoded_len, s_config.device_id, s_config.type_id, x, y))
    {
        return false;
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client,
                                         s_data_pub_topic,
                                         (const char *)payload,
                                         (int)encoded_len,
                                         1,
                                         0);
    ESP_LOGI(TAG,
             "Published msg_id=%d topic=%s len=%u x=%.2f y=%.2f",
             msg_id,
             s_data_pub_topic,
             (unsigned int)encoded_len,
             x,
             y);
    s_last_published_seq = current_seq;
    return true;
}

static bool publish_ping_payload(void)
{
    const size_t target_packet_size = sizeof(s_mqtt_ping_payload);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
    uint32_t sequence = s_ping_sequence++;

    int header_len = snprintf((char *)s_mqtt_ping_payload,
                              sizeof(s_mqtt_ping_payload),
                              "PING|seq=%lu|sent_ms=%llu|size=%u|",
                              (unsigned long)sequence,
                              (unsigned long long)now_ms,
                              (unsigned int)target_packet_size);
    if (header_len < 0 || (size_t)header_len >= target_packet_size)
    {
        ESP_LOGE(TAG, "Failed to format ping payload");
        return false;
    }

    fill_random_bytes(s_mqtt_ping_payload + header_len, target_packet_size - (size_t)header_len);

    int msg_id = esp_mqtt_client_publish(s_mqtt_client,
                                         s_ping_pub_topic,
                                         (const char *)s_mqtt_ping_payload,
                                         (int)target_packet_size,
                                         1,
                                         0);
    ESP_LOGI(TAG,
             "Ping sent msg_id=%d topic=%s seq=%lu size=%u",
             msg_id,
             s_ping_pub_topic,
             (unsigned long)sequence,
             (unsigned int)target_packet_size);
    return true;
}

void mqtt_app_notify_new_data(void)
{
    if (s_mqtt_event_group != 0)
    {
        xEventGroupSetBits(s_mqtt_event_group, MQTT_EVENT_DATA_READY_BIT);
    }
}

static void mqtt_publish_task(void *pvParameters)
{
    (void)pvParameters;
    while (1)
    {
        if (s_mqtt_event_group == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(s_mqtt_event_group,
                                               MQTT_EVENT_CONNECTED_BIT | MQTT_EVENT_DATA_READY_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               portMAX_DELAY);

        if ((bits & MQTT_EVENT_CONNECTED_BIT) == 0)
        {
            continue;
        }

        if (s_mqtt_client != NULL)
        {
            uint32_t current_seq = s_config.data_seq ? *s_config.data_seq : 0;
            if (current_seq != s_last_published_seq)
            {
                if (!encode_device_data_and_publish(current_seq))
                {
                    ESP_LOGE(TAG, "Failed to encode/publish");
                }
                else
                {
                    publish_ping_payload();
                    xEventGroupClearBits(s_mqtt_event_group, MQTT_EVENT_DATA_READY_BIT);
                }
            }
            else
            {
                xEventGroupClearBits(s_mqtt_event_group, MQTT_EVENT_DATA_READY_BIT);
            }
        }
        else
        {
            ESP_LOGW(TAG, "MQTT client not ready");
        }
    }
}

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        if (s_mqtt_event_group != 0)
        {
            xEventGroupSetBits(s_mqtt_event_group, MQTT_EVENT_CONNECTED_BIT);
        }
        if (s_ping_sub_topic[0] != '\0')
        {
            int sub_id = esp_mqtt_client_subscribe(s_mqtt_client, s_ping_sub_topic, 1);
            ESP_LOGI(TAG, "Subscribed to ping topic id=%d topic=%s", sub_id, s_ping_sub_topic);
        }
        if (s_data_sub_topic[0] != '\0')
        {
            int sub_id2 = esp_mqtt_client_subscribe(s_mqtt_client, s_data_sub_topic, 1);
            ESP_LOGI(TAG, "Subscribed to data topic id=%d topic=%s", sub_id2, s_data_sub_topic);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        if (s_mqtt_event_group != 0)
        {
            xEventGroupClearBits(s_mqtt_event_group, MQTT_EVENT_CONNECTED_BIT);
        }
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
    {
        const char *topic = event->topic ? event->topic : "";
        int topic_len = event->topic_len;
        const char *data = event->data ? event->data : "";
        int data_len = event->data_len;

        if (topic_len > 0 && strncmp(topic, s_ping_sub_topic, (size_t)topic_len) == 0)
        {
            unsigned long long sent_ms = 0;
            bool parsed = extract_sent_ms_from_payload(data, data_len, &sent_ms);
            if (!parsed)
            {
                ESP_LOGW(TAG, "Ping reply received but sent_ms marker not found");
                break;
            }

            struct timeval tv;
            gettimeofday(&tv, NULL);
            unsigned long long now_ms = (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)(tv.tv_usec / 1000ULL);

            long long delay_ms = (long long)now_ms - (long long)sent_ms;
            long long one_way_ms = delay_ms >= 0 ? (delay_ms / 2LL) : 0;
            ESP_LOGW(TAG,
                     "MQTT latency topic=%.*s sent_ms=%llu recv_ms=%llu rtt_ms=%lld delay_ms=%lld",
                     topic_len,
                     topic,
                     sent_ms,
                     now_ms,
                     delay_ms,
                     one_way_ms);
        }
        else if (topic_len > 0 && strncmp(topic, s_data_sub_topic, (size_t)topic_len) == 0)
        {
            coordinates_data decoded;
            if (decode_coordinates_payload((const uint8_t *)data, (size_t)data_len, &decoded))
            {
                ESP_LOGI(TAG,
                         "Data received topic=%.*s len=%d device_id=%lu type=%lu x=%.2f y=%.2f ts=%llu",
                         topic_len,
                         topic,
                         data_len,
                         (unsigned long)decoded.device_id,
                         (unsigned long)decoded.type,
                         decoded.x,
                         decoded.y,
                         (unsigned long long)decoded.timestamp_ms);
            }
            else
            {
                ESP_LOGW(TAG, "Data received topic=%.*s len=%d but protobuf decode failed", topic_len, topic, data_len);
            }
        }
        else
        {
            ESP_LOGI(TAG, "MQTT data received topic=%.*s len=%d", topic_len, topic, data_len);
        }
        break;
    }
    default:
        ESP_LOGI(TAG, "MQTT event id=%d", event->event_id);
        break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_id;
    mqtt_event_handler_cb((esp_mqtt_event_handle_t)event_data);
}

void mqtt_app_start(const payload_app_config_t *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "Missing config");
        return;
    }

    s_config = *config;

    snprintf(s_ping_pub_topic, sizeof(s_ping_pub_topic), "esp32/gps/publish/ping/%lu", (unsigned long)s_config.device_id);
    snprintf(s_ping_sub_topic, sizeof(s_ping_sub_topic), "esp32/gps/subcribe/ping/%lu", (unsigned long)s_config.device_id);
    snprintf(s_data_pub_topic, sizeof(s_data_pub_topic), "esp32/gps/publish/data/%lu", (unsigned long)s_config.device_id);
    snprintf(s_data_sub_topic, sizeof(s_data_sub_topic), "esp32/gps/subcribe/data/%lu", (unsigned long)s_config.device_id);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_MQTT_URI,
        .credentials.client_id = NULL,
    };

    ESP_LOGI(TAG, "Initializing MQTT with uri=%s", CONFIG_MQTT_URI);
    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return;
    }

    if (s_mqtt_event_group == 0)
    {
        s_mqtt_event_group = xEventGroupCreate();
    }

    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    esp_mqtt_client_start(s_mqtt_client);
    xTaskCreate(mqtt_publish_task, "mqtt_pub", 6144, NULL, 5, NULL);
}

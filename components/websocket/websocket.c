#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "pb_encode.h"
#include "coordinates_data.pb.h"
#include "coordinates_encoder.h"

#include "esp_websocket_client.h"

#include "websocket.h"
#include "ping_state.h"

#define URL_WS CONFIG_URL_WS
#define WS_PACKET_SIZE_10KB 10240
// #define WS_PING_PACKET_SIZE 2048
#define WS_PING_PACKET_SIZE 4096
// #define WS_PING_PACKET_SIZE 10240

static const char *TAG = "websocket";

static payload_app_config_t s_config = {0};
static esp_websocket_client_handle_t s_ws_client = NULL;
static EventGroupHandle_t s_ws_event_group = 0;
static uint32_t s_last_published_seq = 0;

#define WS_EVENT_CONNECTED_BIT BIT0
#define WS_EVENT_DATA_READY_BIT BIT1

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

static bool payload_contains_ping_seq(const char *data, int data_len, uint32_t sequence)
{
    char marker[32];
    int marker_len = snprintf(marker, sizeof(marker), "PING|seq=%lu|", (unsigned long)sequence);

    if (marker_len <= 0 || marker_len > data_len)
    {
        return false;
    }

    for (int i = 0; i <= data_len - marker_len; i++)
    {
        if (memcmp(data + i, marker, (size_t)marker_len) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool handle_websocket_ping_echo(const esp_websocket_event_data_t *data)
{
    bool matched = false;
    uint32_t expected_sequence = 0;
    size_t expected_len = 0;
    uint64_t sent_ms = 0;
    uint64_t received_ms = 0;

    portENTER_CRITICAL(&s_ws_ping_lock);
    expected_sequence = s_ws_ping_state.sequence;
    expected_len = s_ws_ping_state.payload_len;
    sent_ms = s_ws_ping_state.sent_ms;
    bool waiting_for_echo = s_ws_ping_state.waiting_for_echo;
    portEXIT_CRITICAL(&s_ws_ping_lock);

    if (waiting_for_echo && data->data_ptr && data->data_len > 0)
    {
        bool exact_match = (data->data_len == (int)expected_len) &&
                           (memcmp(data->data_ptr, s_ws_ping_state.payload, expected_len) == 0);
        bool marker_match = payload_contains_ping_seq(data->data_ptr, data->data_len, expected_sequence);

        if (exact_match || marker_match)
        {
            portENTER_CRITICAL(&s_ws_ping_lock);
            s_ws_ping_state.echo_received = true;
            s_ws_ping_state.waiting_for_echo = false;
            s_ws_ping_state.received_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            received_ms = s_ws_ping_state.received_ms;
            portEXIT_CRITICAL(&s_ws_ping_lock);
            matched = true;
        }
    }

    if (matched)
    {
        uint64_t rtt_ms = (received_ms >= sent_ms) ? (received_ms - sent_ms) : 0;
        uint64_t delay_ms = rtt_ms / 2ULL;
        ESP_LOGI(TAG, "Echo payload matched for seq=%lu", (unsigned long)expected_sequence);
        ESP_LOGW(TAG,
                 "WebSocket latency seq=%lu send_ms=%llu recv_ms=%llu rtt_ms=%llu delay_ms=%llu",
                 (unsigned long)expected_sequence,
                 (unsigned long long)sent_ms,
                 (unsigned long long)received_ms,
                 (unsigned long long)rtt_ms,
                 (unsigned long long)delay_ms);
    }

    return matched;
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id)
    {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        if (s_ws_event_group != 0)
        {
            xEventGroupSetBits(s_ws_event_group, WS_EVENT_CONNECTED_BIT);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        if (s_ws_event_group != 0)
        {
            xEventGroupClearBits(s_ws_event_group, WS_EVENT_CONNECTED_BIT);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 10)
        {
            ESP_LOGI(TAG, "WebSocket pong received");
            break;
        }

        if (handle_websocket_ping_echo(data))
        {
            break;
        }

        {
            bool waiting = false;
            portENTER_CRITICAL(&s_ws_ping_lock);
            waiting = s_ws_ping_state.waiting_for_echo;
            portEXIT_CRITICAL(&s_ws_ping_lock);

            if (waiting && data->data_ptr && data->data_len > 0)
            {
                int show = data->data_len > 128 ? 128 : data->data_len;
                ESP_LOGD(TAG, "Ignored WebSocket payload: %.*s", show, data->data_ptr);
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;
    default:
        break;
    }
}

static bool websocket_build_uri(char *uri_buffer, size_t uri_buffer_len)
{
    int written = snprintf(uri_buffer, uri_buffer_len, "%s%lu", URL_WS, (unsigned long)s_config.device_id);
    return written > 0 && (size_t)written < uri_buffer_len;
}

static void start_websocket_ping(uint32_t sequence)
{
    size_t target_packet_size = WS_PING_PACKET_SIZE;
    uint64_t sent_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    int payload_len = snprintf((char *)s_ws_ping_state.payload,
                               sizeof(s_ws_ping_state.payload),
                               "PING|seq=%lu|sent_ms=%llu|size=%u|",
                               (unsigned long)sequence,
                               (unsigned long long)sent_ms,
                               (unsigned int)target_packet_size);

    if (payload_len < 0 || (size_t)payload_len >= target_packet_size)
    {
        ESP_LOGE(TAG, "Failed to build ping payload");
        return;
    }

    fill_random_bytes(s_ws_ping_state.payload + payload_len, target_packet_size - (size_t)payload_len);

    portENTER_CRITICAL(&s_ws_ping_lock);
    s_ws_ping_state.waiting_for_echo = true;
    s_ws_ping_state.echo_received = false;
    s_ws_ping_state.sequence = sequence;
    s_ws_ping_state.payload_len = target_packet_size;
    s_ws_ping_state.sent_ms = sent_ms;
    s_ws_ping_state.received_ms = 0;
    portEXIT_CRITICAL(&s_ws_ping_lock);

    int sent = esp_websocket_client_send_bin(s_ws_client,
                                             (const char *)s_ws_ping_state.payload,
                                             (int)s_ws_ping_state.payload_len,
                                             portMAX_DELAY);
    if (sent < 0)
    {
        ESP_LOGE(TAG, "Failed to send websocket ping payload");

        portENTER_CRITICAL(&s_ws_ping_lock);
        s_ws_ping_state.waiting_for_echo = false;
        portEXIT_CRITICAL(&s_ws_ping_lock);
        return;
    }

    ESP_LOGI(TAG, "Ping sent seq=%lu size=%u bytes", (unsigned long)sequence, (unsigned int)s_ws_ping_state.payload_len);
}

void websocket_app_notify_new_data(void)
{
    if (s_ws_event_group != 0)
    {
        xEventGroupSetBits(s_ws_event_group, WS_EVENT_DATA_READY_BIT);
    }
}

static void send_fake_coordinates_task(void *pvParameters)
{
    (void)pvParameters;

    while (1)
    {
        if (s_ws_event_group == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(s_ws_event_group,
                                               WS_EVENT_CONNECTED_BIT | WS_EVENT_DATA_READY_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               portMAX_DELAY);

        if ((bits & WS_EVENT_CONNECTED_BIT) == 0)
        {
            continue;
        }

        if (esp_websocket_client_is_connected(s_ws_client))
        {
            uint32_t current_seq = s_config.data_seq ? *s_config.data_seq : 0;
            if (current_seq != s_last_published_seq)
            {
                uint8_t payload[256];
                size_t encoded_len = 0;
                float x = s_config.x ? *s_config.x : 0.0f;
                float y = s_config.y ? *s_config.y : 0.0f;

                if (encode_coordinates(payload, sizeof(payload), &encoded_len, s_config.device_id, s_config.type_id, x, y))
                {
                    ESP_LOGI(TAG, "Sending coordinates: X=%.2f Y=%.2f protobuf=%u", x, y, (unsigned int)encoded_len);
                    esp_websocket_client_send_bin(s_ws_client, (const char *)payload, (int)encoded_len, portMAX_DELAY);
                    start_websocket_ping(current_seq);
                    s_last_published_seq = current_seq;
                    xEventGroupClearBits(s_ws_event_group, WS_EVENT_DATA_READY_BIT);
                }
                else
                {
                    ESP_LOGE(TAG, "Failed to encode data");
                }
            }
            else
            {
                xEventGroupClearBits(s_ws_event_group, WS_EVENT_DATA_READY_BIT);
            }
        }
        else
        {
            ESP_LOGW(TAG, "WebSocket not connected");
        }
    }
}

void websocket_app_start(const payload_app_config_t *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "Missing websocket config");
        return;
    }

    s_config = *config;

    char uri[128];
    if (!websocket_build_uri(uri, sizeof(uri)))
    {
        ESP_LOGE(TAG, "Failed to build websocket URI");
        return;
    }

    esp_websocket_client_config_t websocket_cfg = {
        .uri = uri,
    };

    ESP_LOGI(TAG, "Initializing WebSocket with URI: %s", uri);

    s_ws_client = esp_websocket_client_init(&websocket_cfg);
    if (s_ws_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return;
    }

    if (s_ws_event_group == 0)
    {
        s_ws_event_group = xEventGroupCreate();
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)s_ws_client));
    esp_websocket_client_start(s_ws_client);

    xTaskCreate(send_fake_coordinates_task, "send_coordinates_task", 4096, NULL, 5, NULL);
}

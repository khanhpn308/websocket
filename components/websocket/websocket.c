#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

#include "pb_encode.h"
#include "coordinates_data.pb.h"
#include "coordinates_encoder.h"

#include "esp_websocket_client.h"

#include "websocket.h"

#define URL_WS CONFIG_URL_WS
#define WS_PACKET_SIZE_10KB 10240
// #define WS_PING_PACKET_SIZE 2048
#define WS_PING_PACKET_SIZE 4096
// #define WS_PING_PACKET_SIZE 10240

static const char *TAG = "websocket";

static websocket_app_config_t s_config = {0};
static esp_websocket_client_handle_t s_ws_client;

typedef struct
{
    bool waiting_for_echo;
    bool echo_received;
    uint32_t sequence;
    size_t payload_len;
    uint64_t sent_ms;
    uint64_t received_ms;
    uint8_t payload[WS_PACKET_SIZE_10KB];
} payload_ping_state_t;

static payload_ping_state_t s_ws_ping_state = {0};
static portMUX_TYPE s_ws_ping_lock = portMUX_INITIALIZER_UNLOCKED;

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

    portENTER_CRITICAL(&s_ws_ping_lock);
    expected_sequence = s_ws_ping_state.sequence;
    expected_len = s_ws_ping_state.payload_len;
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
            portEXIT_CRITICAL(&s_ws_ping_lock);
            matched = true;
        }
    }

    if (matched)
    {
        ESP_LOGI(TAG, "Echo payload matched for seq=%lu", (unsigned long)expected_sequence);
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
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
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

static void send_fake_coordinates_task(void *pvParameters)
{
    (void)pvParameters;

    uint8_t payload[256];
    size_t encoded_len = 0;

    while (1)
    {
        if (esp_websocket_client_is_connected(s_ws_client))
        {
            float x = s_config.x ? *s_config.x : 0.0f;
            float y = s_config.y ? *s_config.y : 0.0f;

            if (encode_coordinates(payload, sizeof(payload), &encoded_len, s_config.device_id, s_config.type_id, x, y))
            {
                ESP_LOGI(TAG, "Sending coordinates: X=%.2f Y=%.2f protobuf=%u", x, y, (unsigned int)encoded_len);
                esp_websocket_client_send_bin(s_ws_client, (const char *)payload, (int)encoded_len, portMAX_DELAY);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to encode data");
            }
        }
        else
        {
            ESP_LOGW(TAG, "WebSocket not connected");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void websocket_ping_task(void *pvParameters)
{
    (void)pvParameters;

    uint32_t sequence = 1;

    while (1)
    {
        if (esp_websocket_client_is_connected(s_ws_client))
        {
            start_websocket_ping(sequence);

            while (1)
            {
                bool echo_received = false;
                uint64_t sent_ms = 0;
                uint64_t received_ms = 0;
                uint32_t current_sequence = 0;

                portENTER_CRITICAL(&s_ws_ping_lock);
                echo_received = s_ws_ping_state.echo_received;
                sent_ms = s_ws_ping_state.sent_ms;
                received_ms = s_ws_ping_state.received_ms;
                current_sequence = s_ws_ping_state.sequence;
                portEXIT_CRITICAL(&s_ws_ping_lock);

                if (echo_received && current_sequence == sequence)
                {
                    ESP_LOGW(TAG, "WebSocket latency seq=%lu send_ms=%llu recv_ms=%llu delay_ms=%llu",
                             (unsigned long)sequence,
                             (unsigned long long)sent_ms,
                             (unsigned long long)received_ms,
                             (unsigned long long)((received_ms - sent_ms) / 2ULL));
                    sequence++;
                    break;
                }

                if (!esp_websocket_client_is_connected(s_ws_client))
                {
                    ESP_LOGW(TAG, "WebSocket disconnected while waiting for ping echo");
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void websocket_app_start(const websocket_app_config_t *config)
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

    ESP_ERROR_CHECK(esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)s_ws_client));
    esp_websocket_client_start(s_ws_client);

    xTaskCreate(send_fake_coordinates_task, "send_coordinates_task", 4096, NULL, 5, NULL);
#if CONFIG_ENABLE_PAYLOAD_PING
    xTaskCreate(websocket_ping_task, "websocket_ping_task", 4096, NULL, 5, NULL);
#endif
}
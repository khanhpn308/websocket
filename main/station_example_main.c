/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "esp_sntp.h"

#include "pb_encode.h"
#include "coordinates_data.pb.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_websocket_client.h"

#include "random.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY CONFIG_ESP_MAXIMUM_RETRY
#define URL_WS CONFIG_URL_WS
#define DEVICE_ID_NUM CONFIG_DEVICE_ID
#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)
#define DEVICE_ID_STR STRINGIFY(CONFIG_DEVICE_ID)
#define FULL_URI URL_WS DEVICE_ID_STR
#define TYPE_ID CONFIG_TYPE_ID

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif
#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

// Đừng quên include file header được sinh ra từ lệnh compile nanopb
// #include "coordinates_data.pb.h"
static const char *TAG = "wifi station";
bool encode_device_data(uint8_t *out_buffer, size_t max_length, size_t *out_encoded_len, float coord_x, float coord_y)
{

    // 1. KHỞI TẠO ZERO: Sử dụng đúng tên message của bạn + "_init_zero"
    coordinates_data msg = coordinates_data_init_zero;

    // 2. Gán giá trị: Gọi đúng tên biến 100% như trong file .proto
    msg.device_id = DEVICE_ID_NUM;
    msg.type = TYPE_ID;
    msg.x = coord_x;
    msg.y = coord_y;
    struct timeval now;
    gettimeofday(&now, NULL);
    msg.timestamp_ms = (uint64_t)now.tv_sec * 1000ULL + (uint64_t)(now.tv_usec / 1000ULL);

    // 3. Khởi tạo luồng và mã hóa (như hướng dẫn trước)
    pb_ostream_t stream = pb_ostream_from_buffer(out_buffer, max_length);

    bool status = pb_encode(&stream, coordinates_data_fields, &msg);

    if (!status)
    {
        // Xử lý lỗi
        return false;
    }

    *out_encoded_len = stream.bytes_written;
    return true;
}

static void set_timezone_utc_plus_7(void)
{
    setenv("TZ", "UTC-7", 1);
    tzset();
}

static void initialize_sntp_time(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    time_t now = 0;
    struct tm timeinfo = {0};

    for (int retry = 0; retry < 15; retry++)
    {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year >= (2024 - 1900))
        {
            ESP_LOGI(TAG, "Time synchronized: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900,
                     timeinfo.tm_mon + 1,
                     timeinfo.tm_mday,
                     timeinfo.tm_hour,
                     timeinfo.tm_min,
                     timeinfo.tm_sec);
            return;
        }

        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/15)", retry + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGW(TAG, "SNTP sync timeout, timestamps may still be incorrect until time is set");
}

// Biến toàn cục quản lý client
esp_websocket_client_handle_t ws_client;

typedef struct
{
    bool waiting_for_echo;
    bool echo_received;
    uint32_t sequence;
    size_t payload_len;
    uint64_t sent_ms;
    uint64_t received_ms;
    char payload[96];
} websocket_ping_state_t;

static websocket_ping_state_t s_ws_ping_state = {0};
static portMUX_TYPE s_ws_ping_lock = portMUX_INITIALIZER_UNLOCKED;

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

static void start_websocket_ping(uint32_t sequence)
{
    uint64_t sent_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    int payload_len = snprintf(s_ws_ping_state.payload,
                               sizeof(s_ws_ping_state.payload),
                               "PING|seq=%lu|sent_ms=%llu",
                               (unsigned long)sequence,
                               (unsigned long long)sent_ms);

    if (payload_len < 0 || payload_len >= (int)sizeof(s_ws_ping_state.payload))
    {
        ESP_LOGE(TAG, "Failed to build ping payload");
        return;
    }

    portENTER_CRITICAL(&s_ws_ping_lock);
    s_ws_ping_state.waiting_for_echo = true;
    s_ws_ping_state.echo_received = false;
    s_ws_ping_state.sequence = sequence;
    s_ws_ping_state.payload_len = (size_t)payload_len;
    s_ws_ping_state.sent_ms = sent_ms;
    s_ws_ping_state.received_ms = 0;
    portEXIT_CRITICAL(&s_ws_ping_lock);

    int sent = esp_websocket_client_send_text(ws_client,
                                              s_ws_ping_state.payload,
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

    ESP_LOGI(TAG, "Ping sent seq=%lu payload=%s", (unsigned long)sequence, s_ws_ping_state.payload);
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

// 1. Hàm khởi tạo WebSocket (Gọi hàm này sau khi Wi-Fi báo kết nối thành công)
void init_websocket(void)
{
    esp_websocket_client_config_t websocket_cfg = {
        // Thay IP bằng IP máy tính chạy FastAPI (tuyệt đối không dùng localhost/127.0.0.1)
        .uri = FULL_URI,
    };
    ESP_LOGI(TAG, "Initializing WebSocket with URI: %s", FULL_URI);

    ws_client = esp_websocket_client_init(&websocket_cfg);
    if (ws_client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        return;
    }

    ESP_ERROR_CHECK(esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client));
    esp_websocket_client_start(ws_client);
}

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

float x = 0.0f;
float y = 0.0f;
bool data_updated = false;

// 1. Định nghĩa cấu trúc của Task
void random_generator_task(void *pvParameters)
{
    // Task trong FreeRTOS luôn phải chạy trong một vòng lặp vô hạn
    while (1)
    {
        // Sinh giá trị ngẫu nhiên
        x = generate_random_float(0.0f, 100.0f); // Ví dụ dải giá trị từ 0 đến 100
        y = generate_random_float(5.0f, 50.0f);  // Ví dụ dải giá trị từ 5 đến 50

        // In kết quả ra console (monitor)
        //printf("Gia tri tao ra - X: %.2f | Y: %.2f\n", x, y);

        // Báo hiệu có data mới
        data_updated = true;

        // Tạm dừng task này đúng 200ms
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// 2. Task gửi dữ liệu Tọa độ được mã hóa Protobuf khi có data mới
void send_fake_coordinates_task(void *pvParameters)
{
    uint8_t payload[256];
    size_t encoded_len = 0;

    while (1)
    {
        // Chỉ gửi khi có data mới
        if (data_updated)
        {
            if (esp_websocket_client_is_connected(ws_client))
            {
                // Mã hóa dữ liệu Protobuf với tọa độ hiện tại
                if (encode_device_data(payload, sizeof(payload), &encoded_len, x, y))
                {
                    ESP_LOGI(TAG, "Sending: X=%.2f, Y=%.2f, size=%d", x, y, encoded_len);
                    // Gửi dữ liệu Protobuf mã hóa
                    esp_websocket_client_send_bin(ws_client, (const char *)payload, encoded_len, portMAX_DELAY);
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
            // Clear flag sau khi gửi
            data_updated = false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void websocket_ping_task(void *pvParameters)
{
    uint32_t sequence = 1;

    while (1)
    {
        if (esp_websocket_client_is_connected(ws_client))
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
                    // vTaskDelay(pdMS_TO_TICKS(2000));
                    ESP_LOGW(TAG, "WebSocket latency seq=%lu send_ms=%llu recv_ms=%llu delay_ms=%llu",
                             (unsigned long)sequence,
                             (unsigned long long)sent_ms,
                             (unsigned long long)received_ms,
                             (unsigned long long)((received_ms - sent_ms) / 2ULL));
                    sequence++;
                    break;
                }

                if (!esp_websocket_client_is_connected(ws_client))
                {
                    ESP_LOGW(TAG, "WebSocket disconnected while waiting for ping echo");
                    break;
                }

                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL)
    {
        /* If you only want to open more logs in the wifi module, you need to make the max level greater than the default level,
         * and call esp_log_level_set() before esp_wifi_init() to improve the log level of the wifi module. */
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();
    set_timezone_utc_plus_7();
    initialize_sntp_time();
    init_websocket();
    xTaskCreate(random_generator_task, "random_task", 2048, NULL, 5, NULL);
    xTaskCreate(send_fake_coordinates_task, "send_coordinates_task", 4096, NULL, 5, NULL);
#if CONFIG_ENABLE_WEBSOCKET_PING
    xTaskCreate(websocket_ping_task, "websocket_ping_task", 4096, NULL, 5, NULL);
#endif
}

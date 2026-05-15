#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "mqtt_client.h"
#include "pb_encode.h"
#include "coordinates_data.pb.h"
#include "coordinates_encoder.h"

#include "mqtt.h"

static const char *TAG = "mqtt_app";

static payload_app_config_t s_config = {0};
static esp_mqtt_client_handle_t s_mqtt_client = NULL;

static bool encode_device_data_and_publish(void)
{
    uint8_t payload[256];
    size_t encoded_len = 0;
    float x = s_config.x ? *s_config.x : 0.0f;
    float y = s_config.y ? *s_config.y : 0.0f;

    if (!encode_coordinates(payload, sizeof(payload), &encoded_len, s_config.device_id, s_config.type_id, x, y))
    {
        return false;
    }

    char topic[64];
    int t = snprintf(topic, sizeof(topic), "devices/%lu/coords", (unsigned long)s_config.device_id);
    if (t <= 0)
        strcpy(topic, "devices/coords");

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, (const char *)payload, (int)encoded_len, 1, 0);
    ESP_LOGI(TAG, "Published msg_id=%d topic=%s len=%u", msg_id, topic, (unsigned int)encoded_len);
    return true;
}

static void mqtt_publish_task(void *pvParameters)
{
    (void)pvParameters;
    while (1)
    {
        if (s_mqtt_client != NULL)
        {
            if (!encode_device_data_and_publish())
            {
                ESP_LOGE(TAG, "Failed to encode/publish");
            }
        }
        else
        {
            ESP_LOGW(TAG, "MQTT client not ready");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mqtt_app_start(const payload_app_config_t *config)
{
    if (config == NULL)
    {
        ESP_LOGE(TAG, "Missing config");
        return;
    }

    s_config = *config;

    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_MQTT_URI,
        .client_id = client_id;
};

ESP_LOGI(TAG, "Initializing MQTT with uri=%s", CONFIG_MQTT_URI);
s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
if (s_mqtt_client == NULL)
{
    ESP_LOGE(TAG, "Failed to init MQTT client");
    return;
}

esp_mqtt_client_start(s_mqtt_client);
xTaskCreate(mqtt_publish_task, "mqtt_pub", 4096, NULL, 5, NULL);

}
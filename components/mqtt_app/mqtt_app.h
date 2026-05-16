void mqtt_app_notify_new_data(void);
#ifndef MQTT_APP_H
#define MQTT_APP_H

#include "websocket.h"

void mqtt_app_start(const payload_app_config_t *config);

#endif
void websocket_app_notify_new_data(void);
#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    const float *x;
    const float *y;
    volatile uint32_t *data_seq;
    uint32_t device_id;
    uint32_t type_id;
} payload_app_config_t;

void websocket_app_start(const payload_app_config_t *config);

#endif
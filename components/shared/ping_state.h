
#pragma once
#include "freertos/FreeRTOS.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef WS_PACKET_SIZE_10KB
#define WS_PACKET_SIZE_10KB 10240
#endif

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

extern payload_ping_state_t s_ws_ping_state;
extern portMUX_TYPE s_ws_ping_lock;

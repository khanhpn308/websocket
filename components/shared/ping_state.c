#include "freertos/FreeRTOS.h"
#include "ping_state.h"

payload_ping_state_t s_ws_ping_state = {0};
portMUX_TYPE s_ws_ping_lock = portMUX_INITIALIZER_UNLOCKED;

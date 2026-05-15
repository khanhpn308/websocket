#ifndef COORDINATES_ENCODER_H
#define COORDINATES_ENCODER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Encode coordinates_data protobuf message into out_buffer.
// Returns true on success and sets *out_encoded_len.
bool encode_coordinates(uint8_t *out_buffer, size_t max_length, size_t *out_encoded_len,
                        uint32_t device_id, uint32_t type_id,
                        float coord_x, float coord_y);

#endif

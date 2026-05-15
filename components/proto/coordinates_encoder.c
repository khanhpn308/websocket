#include "coordinates_encoder.h"
#include "pb_encode.h"
#include "coordinates_data.pb.h"
#include <sys/time.h>

bool encode_coordinates(uint8_t *out_buffer, size_t max_length, size_t *out_encoded_len,
                        uint32_t device_id, uint32_t type_id,
                        float coord_x, float coord_y)
{
    if (out_buffer == NULL || out_encoded_len == NULL)
        return false;

    coordinates_data msg = coordinates_data_init_zero;
    msg.device_id = device_id;
    msg.type = type_id;
    msg.x = coord_x;
    msg.y = coord_y;

    struct timeval now;
    gettimeofday(&now, NULL);
    msg.timestamp_ms = (uint64_t)now.tv_sec * 1000ULL + (uint64_t)(now.tv_usec / 1000ULL);

    pb_ostream_t stream = pb_ostream_from_buffer(out_buffer, max_length);
    if (!pb_encode(&stream, coordinates_data_fields, &msg))
    {
        return false;
    }

    *out_encoded_len = stream.bytes_written;
    return true;
}

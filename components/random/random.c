#include "random.h"
#include "esp_random.h" // Thư viện cấp phát số ngẫu nhiên phần cứng của ESP-IDF
#include <stdint.h>

float generate_random_float(float min, float max) {
    // esp_random() trả về một số uint32_t từ 0 đến UINT32_MAX (4294967295)
    uint32_t hardware_rand = esp_random();
    
    // Chia cho UINT32_MAX ép kiểu sang float để lấy tỷ lệ phân bố [0.0, 1.0]
    float scale = (float)hardware_rand / 4294967295.0f; 
    
    // Thu phóng về khoảng [min, max]
    return min + scale * (max - min);
}
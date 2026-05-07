# Hướng dẫn Toàn diện: Tích hợp và Sử dụng Nanopb trong dự án ESP-IDF

## Tóm tắt
Tài liệu này cung cấp quy trình chuẩn để tích hợp thư viện **Nanopb** (Protocol Buffers cho hệ thống nhúng) vào bất kỳ dự án ESP-IDF nào. Mục tiêu là đóng gói các dữ liệu nghiệp vụ thành mảng byte (nhị phân) siêu gọn nhẹ để truyền tải qua MQTT, WebSockets, UART, hoặc lưu trữ. 

Quy trình cốt lõi bao gồm: **Khai báo môi trường $\rightarrow$ Viết file Schema (.proto) $\rightarrow$ Sinh mã C tự động $\rightarrow$ Tích hợp mã C vào dự án $\rightarrow$ Xử lý bộ nhớ (Buffer) để Mã hóa/Giải mã.**

---

## Phần 1: Cấu hình môi trường và Tích hợp thư viện

Sử dụng ESP-IDF Component Manager là phương pháp tối ưu nhất để quản lý sự phụ thuộc, giúp dự án nhẹ và tránh rác mã nguồn.

### Bước 1: Khai báo thư viện (idf_component.yml)
Tạo tệp văn bản có tên `idf_component.yml` đặt bên trong thư mục `main/` của dự án:
```yaml
dependencies:
  nikas-belogolov/nanopb: "^0.4.9"
```

### Bước 2: Liên kết thư viện (CMakeLists.txt)
Cập nhật tệp `main/CMakeLists.txt` bằng cách dùng từ khóa `PRIV_REQUIRES` để liên kết nội bộ `nanopb` vào thành phần `main`.

```cmake
# Lưu ý: Lát nữa chúng ta sẽ thêm file tự động sinh (.pb.c) vào mục SRCS
idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES mqtt esp_wifi nanopb)
```

### Bước 3: Tải thư viện
Chạy lệnh sau tại thư mục gốc của dự án để ESP-IDF tự động tải `nanopb` về thư mục ẩn `managed_components`:
```bash
idf.py reconfigure
```

---

## Phần 2: Xây dựng Schema và Sinh mã nguồn C

### Bước 1: Định nghĩa cấu trúc tin nhắn (.proto)
Tạo một file `.proto` (ví dụ: `app_message.proto`) trong thư mục `main/`. Hãy tuân thủ nguyên tắc: ưu tiên dùng số nguyên không dấu (`uint32`, `uint64`) cho các giá trị luôn dương để tối ưu hóa bộ nhớ RAM.

```protobuf
syntax = "proto3";

// Cấu trúc gói tin tổng quát cho mọi dự án
message DeviceData {
    uint32 device_id = 1;
    float value_1 = 2;
    float value_2 = 3;
    bool status = 4;
    uint32 error_code = 5;
    uint64 timestamp_ms = 6;
}
```

### Bước 2: Lệnh sinh file mã C (Bắt buộc)
Để vi điều khiển hiểu được file `.proto`, bạn cần dùng công cụ biên dịch (Compiler) sinh ra mã C. 

1. Cài đặt công cụ trên máy tính của bạn (Mở Terminal/Command Prompt):
```bash
pip install protobuf nanopb
```
2. Di chuyển vào thư mục `main/` (nơi chứa file `app_message.proto`) và chạy lệnh sinh mã:
```bash
nanopb_generator app_message.proto
```
*Kết quả:* Hệ thống sẽ tự động tạo ra 2 file: `app_message.pb.h` và `app_message.pb.c`.

### Bước 3: Tích hợp file sinh ra vào Source Code
Bạn **phải** khai báo file `.pb.c` vừa sinh ra vào hệ thống build để trình biên dịch tổng hợp nó. Mở lại file `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "app_main.c" "app_message.pb.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES mqtt esp_wifi nanopb)
```

---

## Phần 3: Mã hóa (Encode) và Thao tác với Con trỏ / Buffer

**Insight:** Vi điều khiển có bộ nhớ RAM giới hạn (ví dụ: 512KB). Do đó, dữ liệu sau khi mã hóa phải được lưu vào một vùng nhớ đệm (mảng Buffer). Thư viện sẽ dùng khái niệm "Stream" để rót dữ liệu từ Struct C vào mảng Buffer này.

Trong file `.c` nghiệp vụ của bạn (ví dụ `app_main.c`), include các header cần thiết:

```c
#include <stdio.h>
#include <string.h>
#include "pb_encode.h"      // Thư viện mã hóa nanopb
#include "app_message.pb.h" // File header cấu trúc dữ liệu vừa sinh ra

/**
 * Hàm mã hóa dữ liệu thành mảng byte
 * @param out_buffer: Con trỏ trỏ đến mảng chứa dữ liệu đầu ra
 * @param max_length: Kích thước tối đa của mảng đầu ra để tránh tràn RAM
 * @param out_encoded_len: Con trỏ lưu kích thước thực tế sau khi mã hóa
 * @return: true nếu thành công, false nếu thất bại
 */
bool encode_device_data(uint8_t *out_buffer, size_t max_length, size_t *out_encoded_len) {
    
    // 1. KHỞI TẠO ZERO (Quan trọng): Dọn sạch rác trong RAM trước khi gán
    DeviceData msg = DeviceData_init_zero;
    
    // 2. Gán giá trị vào cấu trúc
    msg.device_id = 105;
    msg.value_1 = 24.5f;
    msg.value_2 = 1.12f;
    msg.status = true;
    msg.error_code = 0;
    msg.timestamp_ms = 1710000000000ULL;

    // 3. Tạo luồng ghi (Stream) liên kết với mảng Buffer
    pb_ostream_t stream = pb_ostream_from_buffer(out_buffer, max_length);

    // 4. Thực thi mã hóa
    bool status = pb_encode(&stream, DeviceData_fields, &msg);

    if (!status) {
        printf("Lỗi mã hóa Protobuf: %s\n", PB_GET_ERROR(&stream));
        return false;
    }

    // 5. Trả về kích thước gói tin thực tế thông qua con trỏ
    *out_encoded_len = stream.bytes_written;
    return true;
}
```

**Cách gọi hàm trong thực tế:**

```c
void app_main(void) {
    // Cấp phát 1 mảng buffer 128 byte (128 ô nhớ)
    uint8_t payload_buffer[128]; 
    size_t actual_length = 0;

    // Gọi hàm mã hóa, truyền địa chỉ của mảng và biến chiều dài vào
    if (encode_device_data(payload_buffer, sizeof(payload_buffer), &actual_length)) {
        printf("Mã hóa thành công! Kích thước: %zu bytes\n", actual_length);
        
        // Truyền mảng payload_buffer lên Server (FastAPI / MQTT) tại đây
        // mqtt_send_data(topic, payload_buffer, actual_length);
    }
}
```

---

## Phần 4: Giải mã (Decode) - Tùy chọn cho thiết bị nhận

Nếu thiết bị của bạn cần nhận cấu hình từ Server (Downlink), bạn làm ngược lại với `pb_decode`.

```c
#include "pb_decode.h"

bool decode_server_command(const uint8_t *in_buffer, size_t data_length) {
    
    DeviceData msg = DeviceData_init_zero; // Luôn khởi tạo Zero
    
    // Tạo luồng đọc từ mảng nhị phân
    pb_istream_t stream = pb_istream_from_buffer(in_buffer, data_length);
    
    // Giải mã mảng byte ngược lại thành Struct C
    bool status = pb_decode(&stream, DeviceData_fields, &msg);
    
    if (!status) {
        printf("Lỗi giải mã: %s\n", PB_GET_ERROR(&stream));
        return false;
    }
    
    printf("Server gửi lệnh cho Node ID: %lu\n", (unsigned long)msg.device_id);
    return true;
}
```

---

## Phần 5: Đóng gói giao thức tầng ứng dụng (Frame Data - Nâng cao)

Khi truyền tải trong mạng nội bộ nhà máy lớn, để Server nhận diện và phân loại dễ dàng trước khi ném vào bộ giải mã Protobuf, bạn nên gói `payload_buffer` vào một Khung (Frame) có cấu trúc cố định.

```c
// Cấu trúc Header cố định, __attribute__((packed)) ngăn C compiler tự động chèn byte rác
typedef struct __attribute__((packed)) {
    uint8_t magic_byte;  // Byte nhận dạng (VD: 0xAA)
    uint8_t msg_type;    // Phân loại (VD: 1 = Telemetry, 2 = Alarm)
    uint16_t payload_len;// Chiều dài của mảng Protobuf phía sau
} frame_header_t;
```

**Ghép Header và Payload trước khi gửi:**
```c
uint8_t final_tx_buffer[256]; // Buffer tổng để gửi qua mạng
size_t payload_len = 0;

// 1. Lấy dữ liệu Payload Protobuf
encode_device_data(payload_buffer, 128, &payload_len);

// 2. Tạo Header
frame_header_t header = {
    .magic_byte = 0xAA,
    .msg_type = 1,
    .payload_len = (uint16_t)payload_len
};

// 3. Nối Header và Payload vào Buffer tổng
memcpy(final_tx_buffer, &header, sizeof(header));
memcpy(final_tx_buffer + sizeof(header), payload_buffer, payload_len);

// Kích thước tổng cộng cần gửi: sizeof(header) + payload_len
```
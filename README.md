# ESP32 WebSocket Station

Project ESP32 này là một ứng dụng Wi-Fi Station dùng ESP-IDF để:

- Kết nối vào Wi-Fi bằng SSID/password cấu hình trong `menuconfig`.
- Đồng bộ thời gian qua SNTP và đặt múi giờ UTC+7.
- Đóng gói dữ liệu tọa độ bằng nanopb protobuf và gửi qua WebSocket.
- Gửi ping ở mức ứng dụng để đo độ trễ echo và in log ra terminal.
- Cho phép bật/tắt task ping bằng một cờ cấu hình riêng.

## Tính năng chính

- Wi-Fi Station: tự kết nối vào Access Point đã cấu hình.
- Timestamp chuẩn: timestamp của dữ liệu không còn lấy từ uptime, mà lấy từ thời gian hệ thống sau khi SNTP sync.
- Múi giờ Việt Nam: hiển thị theo UTC+7.
- Gửi dữ liệu protobuf: mỗi gói dữ liệu chứa `device_id`, `type`, `x`, `y`, `timestamp_ms`.
- WebSocket app ping: gửi chuỗi `PING|seq=...|sent_ms=...` và chờ server echo lại để đo độ trễ.
- Lọc log: chỉ log pong, ping echo và latency; các frame không liên quan được hạ mức debug.

## Cấu hình

Mở `idf.py menuconfig` rồi vào mục `Example Configuration` để chỉnh:

- `WiFi SSID`
- `WiFi Password`
- `Websocket URL`
- `Device ID`
- `Type ID`
- `Enable WebSocket ping task`
- `Maximum retry`

### Ý nghĩa một số cấu hình

- `Websocket URL`: địa chỉ WebSocket server, ví dụ `ws://<ip-may-chu>:<port>/...`.
- `Device ID`: ID thiết bị được nhúng vào protobuf.
- `Type ID`: mã loại dữ liệu, ví dụ map / voltage / current / temperature / vibration.
- `Enable WebSocket ping task`: bật hoặc tắt task ping đo độ trễ.

## Build và chạy

1. Mở project bằng ESP-IDF.
2. Chạy `idf.py menuconfig` để cấu hình Wi-Fi và WebSocket.
3. Build và flash:

```bash
idf.py build flash monitor
```

4. Thoát monitor bằng `Ctrl-]`.

## Luồng hoạt động

1. ESP32 khởi động ở chế độ STA.
2. Kết nối Wi-Fi và chờ có IP.
3. Thiết lập múi giờ UTC+7 và đồng bộ SNTP.
4. Khởi tạo WebSocket client.
5. Task tạo tọa độ ngẫu nhiên chạy định kỳ.
6. Task gửi protobuf khi có dữ liệu mới.
7. Nếu bật ping, task ping sẽ gửi payload dạng text để server echo lại và tính độ trễ.

## Log mong đợi

Khi kết nối thành công, monitor thường sẽ thấy các log như:

```text
wifi station: got ip:192.168.x.x
wifi station: Time synchronized: 2026-05-07 14:xx:xx
wifi station: WebSocket connected
wifi station: Ping sent seq=1 payload=PING|seq=1|sent_ms=...
wifi station: Echo payload matched for seq=1
wifi station: WebSocket latency seq=1 send_ms=... recv_ms=... delay_ms=...
```

## Ghi chú kỹ thuật

- Timestamp dữ liệu protobuf lấy từ `gettimeofday()` sau khi SNTP đã sync, nên không còn hiện kiểu năm 1970.
- Múi giờ được cấu hình bằng `TZ=UTC-7` trong code.
- Chu kỳ ping nằm trong `websocket_ping_task()`; muốn ping nhanh hơn thì giảm delay ở vòng lặp của task này.
- Nếu server trả về JSON wrapper chứa chuỗi `PING|seq=...|`, code hiện tại vẫn nhận ra và tính latency.

## Hỗ trợ target

Project hỗ trợ các target ESP32, ESP32-C2, ESP32-C3, ESP32-C5, ESP32-C6, ESP32-C61, ESP32-S2, ESP32-S3, ESP32-P4 và ESP32-H2.

## Xử lý lỗi thường gặp

- Không có IP: kiểm tra SSID/password và tín hiệu Wi-Fi.
- Không sync được thời gian: kiểm tra kết nối Internet để ESP32 truy cập được NTP server.
- Không thấy latency log: kiểm tra server WebSocket có echo lại payload ping hay không, và thử giảm mức lọc log nếu cần.

## Tuỳ chỉnh nhanh (protocol, kích thước ping, topic MQTT)

- **Thay đổi protocol (WebSocket / MQTT):**
  - Mở file `main/station_example_main.c` và thay đổi giá trị của biến `current_protocol` thành `websocket_protocol` hoặc `mqtt_protocol` trước khi gọi `websocket_app_start()` / `mqtt_app_start()`.
  - Nếu bạn muốn chọn protocol ở thời gian build/runtime, hãy thêm một cờ cấu hình (`Kconfig`) hoặc sử dụng `menuconfig` để expose một `CONFIG_` và đọc giá trị đó trong `app_main()`.

- **Đổi kích thước payload ping (MQTT):**
  - File: [components/mqtt/mqtt.c](components/mqtt/mqtt.c)
  - Mở hàm `mqtt_ping_task()` và chỉnh kích thước buffer `char payload[32];` thành `char payload[64];` (hoặc lớn hơn nếu muốn). Đồng thời đảm bảo `snprintf()` không vượt kích thước buffer.
  - Tương tự với WebSocket ping, sửa trong component WebSocket (thường là `components/websocket/*`) thay đổi buffer và định dạng payload nếu cần.

- **Thay đổi topic MQTT (publish / subscribe):**
  - File: [components/mqtt/mqtt.c](components/mqtt/mqtt.c)
  - Các topic mặc định hiện tại:
    - Ping publish: `esp32/gps/publish/ping/{deviceid}` (`s_ping_pub_topic`)
    - Ping subscribe: `esp32/gps/subcribe/ping/{deviceid}` (`s_ping_sub_topic`)
    - Data publish: `esp32/gps/publish/data/{deviceid}` (`s_data_pub_topic`)
    - Data subscribe: `esp32/gps/subcribe/data/{deviceid}` (`s_data_sub_topic`)
  - Để thay đổi, sửa chuỗi format trong `snprintf()` khi khởi tạo các biến trên (tìm `snprintf(..."esp32/gps/.../", ...)`).
  - Sau chỉnh sửa, build lại project:
    ```bash
    idf.py build flash monitor
    ```

- **Gợi ý:** nếu muốn topic có cấu trúc khác (ví dụ `myorg/dev/{id}/ping`), cập nhật cả phần publish và subscribe để server và thiết bị đồng bộ.

Nếu muốn, tôi có thể chuyển các format topic và kích thước ping sang `Kconfig` để dễ cấu hình qua `menuconfig` — bạn có muốn tôi làm điều đó không?

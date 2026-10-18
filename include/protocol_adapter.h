/*
 * protocol_adapter.h
 *
 * Giao diện và cấu trúc lớp Protocol Adapter.
 * Cung cấp sẵn theo đặc tả Mục 3.5 của đề bài.
 */

#ifndef PROTOCOL_ADAPTER_H
#define PROTOCOL_ADAPTER_H

#include <stddef.h>
#include <stdint.h>
#include "sensor_types.h"

/* Kết quả giải mã frame từ byte stream */
typedef enum {
    PARSE_ERROR     = -1, /* Frame hỏng: sai magic, độ dài, CRC, giá trị */
    PARSE_NEED_MORE =  0, /* Chưa đủ byte để tạo thành một frame */
    PARSE_OK        =  1  /* Đã giải mã đúng một frame hoàn chỉnh */
} parse_result_t;

/* Cấu trúc mô tả một protocol adapter */
typedef struct {
    const char *name;     /* Tên giao thức: "TBP", "HLP", "EVP", "SGP" */
    uint16_t port_offset; /* Cổng lắng nghe = cổng gốc P + port_offset */

    /* Giải mã tối đa một frame ở đầu bộ đệm buf.
     * PARSE_OK:        Điền thông tin vào *out, *consumed = số byte đã dùng.
     * PARSE_NEED_MORE: Giữ nguyên byte, chờ nhận thêm dữ liệu qua socket.
     * PARSE_ERROR:     *consumed = số byte cần bỏ để tái đồng bộ luồng byte.
     */
    parse_result_t (*parse)(const uint8_t *buf, size_t len,
                            sensor_data_t *out, size_t *consumed);

    /* Tạo phản hồi gửi lại sensor node (nếu giao thức yêu cầu ACK/NAK).
     * Trả về số byte phản hồi được ghi vào reply; trả 0 nếu không có phản hồi.
     */
    size_t (*build_reply)(parse_result_t result, uint8_t *reply, size_t cap);
} protocol_adapter_t;

/* Khai báo 4 protocol adapter chính thức */
extern const protocol_adapter_t adapter_tbp;
extern const protocol_adapter_t adapter_hlp;
extern const protocol_adapter_t adapter_evp;
extern const protocol_adapter_t adapter_sgp;

/* Bảng tra cứu protocol adapter theo thứ tự port_offset (0..3) */
extern const protocol_adapter_t * const ADAPTERS[4];

/* Hàm tiện ích tính CRC-16/CCITT-FALSE cho giao thức SGP */
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

#endif /* PROTOCOL_ADAPTER_H */


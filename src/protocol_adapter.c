/*
 * protocol_adapter.c
 *
 * Hiện thực lớp Protocol Adapter cho 4 giao thức cảm biến:
 * 1. TBP (Temperature Binary Protocol) - Fixed 18 bytes, little-endian.
 * 2. HLP (Humidity Line Protocol) - ASCII text, max 64 bytes kết thúc bằng '\n'.
 * 3. EVP (Event TLV Protocol) - Type-Length-Value, big-endian.
 * 4. SGP (Smoke/Gas Protocol) - Fixed 11 bytes, big-endian, CRC-16/CCITT-FALSE, ACK/NAK.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include "protocol_adapter.h"
#include "sensor_config.h"

/*
 * Thuật toán CRC-16/CCITT-FALSE:
 * Đa thức: 0x1021
 * Giá trị khởi tạo: 0xFFFF
 * Không đảo bit (RefIn = false, RefOut = false)
 * XOR đầu ra: 0x0000
 * Vector kiểm tra: "123456789" -> 0x29B1
 */
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

/* =========================================================================
 * 1. TBP – Temperature Binary Protocol (Cổng P + 0)
 * Khung 18 byte cố định, little-endian:
 *   [0..1]   : id (uint16, little-endian)
 *   [2..9]   : value (IEEE 754 double 8 byte, little-endian)
 *   [10..17] : ts (int64, little-endian)
 * ========================================================================= */
static parse_result_t parse_tbp(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed) {
    if (len < 18) {
        *consumed = 0;
        return PARSE_NEED_MORE;
    }

    /* Đọc uint16_t little-endian cho id */
    uint16_t id = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);

    /* Đọc 8 byte IEEE 754 double little-endian */
    uint64_t val_bits = 0;
    for (int i = 0; i < 8; i++) {
        val_bits |= ((uint64_t)buf[2 + i]) << (i * 8);
    }
    double value;
    memcpy(&value, &val_bits, sizeof(double));

    /* Đọc int64_t little-endian cho timestamp */
    int64_t ts = 0;
    for (int i = 0; i < 8; i++) {
        ts |= ((int64_t)buf[10 + i]) << (i * 8);
    }

    /* Kiểm tra tính hợp lệ của dải nhiệt độ: -40.0°C đến +85.0°C */
    if (value < -40.0 || value > 85.0 || ts <= 0) {
        /* Byte không hợp lệ, bỏ 1 byte để tìm frame hợp lệ kế tiếp */
        *consumed = 1;
        return PARSE_ERROR;
    }

    out->id = id;
    out->type = SENSOR_TEMPERATURE;
    out->value = value;
    out->ts = (time_t)ts;
    *consumed = 18;
    return PARSE_OK;
}

static size_t reply_tbp(parse_result_t result, uint8_t *reply, size_t cap) {
    (void)result;
    (void)reply;
    (void)cap;
    return 0; /* TBP không có phản hồi */
}

const protocol_adapter_t adapter_tbp = {
    .name = "TBP",
    .port_offset = 0,
    .parse = parse_tbp,
    .build_reply = reply_tbp
};

/* =========================================================================
 * 2. HLP – Humidity Line Protocol (Cổng P + 1)
 * Dòng văn bản ASCII kết thúc bằng LF (0x0A), tối đa 64 byte kể cả LF.
 * Cú pháp: "HUM <id> <value> <ts>\n"
 * ========================================================================= */
static parse_result_t parse_hlp(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed) {
    /* Tìm ký tự LF ('\n') trong bộ đệm */
    size_t lf_pos = 0;
    bool found_lf = false;

    for (size_t i = 0; i < len && i < 64; i++) {
        if (buf[i] == '\n') {
            lf_pos = i;
            found_lf = true;
            break;
        }
    }

    if (!found_lf) {
        if (len < 64) {
            *consumed = 0;
            return PARSE_NEED_MORE;
        }
        /* Dài quá 64 byte mà chưa thấy LF -> Lỗi frame */
        *consumed = 1; /* Bỏ byte đầu để đồng bộ */
        return PARSE_ERROR;
    }

    /* Đã tìm thấy LF tại lf_pos (< 64) */
    size_t frame_len = lf_pos + 1;
    char line[65];
    memcpy(line, buf, lf_pos);
    line[lf_pos] = '\0';

    /* Bỏ qua ký tự CR '\r' nếu có trước LF */
    if (lf_pos > 0 && line[lf_pos - 1] == '\r') {
        line[lf_pos - 1] = '\0';
    }

    unsigned int id_val = 0;
    double humidity = 0.0;
    int64_t ts_val = 0;

    int matched = sscanf(line, "HUM %u %lf %" SCNd64, &id_val, &humidity, &ts_val);
    if (matched != 3 || humidity < 0.0 || humidity > 100.0 || ts_val <= 0) {
        *consumed = frame_len;
        return PARSE_ERROR;
    }

    out->id = (sensor_id_t)id_val;
    out->type = SENSOR_HUMIDITY;
    out->value = humidity;
    out->ts = (time_t)ts_val;
    *consumed = frame_len;
    return PARSE_OK;
}

static size_t reply_hlp(parse_result_t result, uint8_t *reply, size_t cap) {
    (void)result;
    (void)reply;
    (void)cap;
    return 0; /* HLP không có phản hồi */
}

const protocol_adapter_t adapter_hlp = {
    .name = "HLP",
    .port_offset = 1,
    .parse = parse_hlp,
    .build_reply = reply_hlp
};

/* =========================================================================
 * 3. EVP – Event TLV Protocol (Cổng P + 2)
 * Nhị phân big-endian:
 * Type 0x01 (Motion): len 6 -> id (uint16_be) + ts (uint32_be) = 8 byte
 * Type 0x02 (Door):   len 7 -> id (uint16_be) + state (uint8) + ts (uint32_be) = 9 byte
 * ========================================================================= */
static parse_result_t parse_evp(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed) {
    if (len < 2) {
        *consumed = 0;
        return PARSE_NEED_MORE;
    }

    uint8_t type = buf[0];
    uint8_t length = buf[1];
    size_t total_len = 2 + length;

    if (type == 0x01) { /* Motion */
        if (length != 6) {
            *consumed = 1;
            return PARSE_ERROR;
        }
        if (len < total_len) {
            *consumed = 0;
            return PARSE_NEED_MORE;
        }

        uint16_t id = ((uint16_t)buf[2] << 8) | buf[3];
        uint32_t ts = ((uint32_t)buf[4] << 24) |
                      ((uint32_t)buf[5] << 16) |
                      ((uint32_t)buf[6] << 8)  |
                      buf[7];

        out->id = id;
        out->type = SENSOR_MOTION;
        out->value = 1.0; /* 1 = phát hiện chuyển động */
        out->ts = (time_t)ts;
        *consumed = total_len;
        return PARSE_OK;
    }
    else if (type == 0x02) { /* Door */
        if (length != 7) {
            *consumed = 1;
            return PARSE_ERROR;
        }
        if (len < total_len) {
            *consumed = 0;
            return PARSE_NEED_MORE;
        }

        uint16_t id = ((uint16_t)buf[2] << 8) | buf[3];
        uint8_t state = buf[4]; /* 0 = đóng, 1 = mở */
        if (state > 1) {
            *consumed = total_len;
            return PARSE_ERROR;
        }

        uint32_t ts = ((uint32_t)buf[5] << 24) |
                      ((uint32_t)buf[6] << 16) |
                      ((uint32_t)buf[7] << 8)  |
                      buf[8];

        out->id = id;
        out->type = SENSOR_DOOR;
        out->value = (double)state;
        out->ts = (time_t)ts;
        *consumed = total_len;
        return PARSE_OK;
    }

    /* Type không xác định */
    *consumed = 1;
    return PARSE_ERROR;
}

static size_t reply_evp(parse_result_t result, uint8_t *reply, size_t cap) {
    (void)result;
    (void)reply;
    (void)cap;
    return 0; /* EVP không có phản hồi */
}

const protocol_adapter_t adapter_evp = {
    .name = "EVP",
    .port_offset = 2,
    .parse = parse_evp,
    .build_reply = reply_evp
};

/* =========================================================================
 * 4. SGP – Smoke/Gas Protocol (Cổng P + 3)
 * Khung 11 byte cố định, big-endian:
 *   [0]   : Magic = 0x7E
 *   [1..2]: id (uint16_be)
 *   [3..4]: ppm (uint16_be, 0..10000 ppm)
 *   [5..8]: ts (uint32_be)
 *   [9..10]: CRC-16 (uint16_be) của byte offset [1..8]
 * Phản hồi: ACK (0x06) khi PARSE_OK, NAK (0x15) khi PARSE_ERROR.
 * ========================================================================= */
static parse_result_t parse_sgp(const uint8_t *buf, size_t len,
                                sensor_data_t *out, size_t *consumed) {
    if (len < 11) {
        *consumed = 0;
        return PARSE_NEED_MORE;
    }

    /* Kiểm tra byte Magic 0x7E */
    if (buf[0] != 0x7E) {
        *consumed = 1; /* Trượt 1 byte để tìm magic 0x7E tiếp theo */
        return PARSE_ERROR;
    }

    /* Đọc CRC từ frame */
    uint16_t frame_crc = ((uint16_t)buf[9] << 8) | buf[10];

    /* Tính toán CRC trên 8 byte payload (từ offset 1 đến offset 8) */
    uint16_t calculated_crc = crc16_ccitt_false(&buf[1], 8);

    if (frame_crc != calculated_crc) {
        *consumed = 11; /* Bỏ cả frame lỗi */
        return PARSE_ERROR;
    }

    uint16_t id = ((uint16_t)buf[1] << 8) | buf[2];
    uint16_t ppm = ((uint16_t)buf[3] << 8) | buf[4];
    uint32_t ts = ((uint32_t)buf[5] << 24) |
                  ((uint32_t)buf[6] << 16) |
                  ((uint32_t)buf[7] << 8)  |
                  buf[8];

    if (ppm > 10000) {
        *consumed = 11;
        return PARSE_ERROR;
    }

    out->id = id;
    out->type = SENSOR_SMOKE_GAS;
    out->value = (double)ppm;
    out->ts = (time_t)ts;
    *consumed = 11;
    return PARSE_OK;
}

static size_t reply_sgp(parse_result_t result, uint8_t *reply, size_t cap) {
    if (cap < 1) return 0;

    if (result == PARSE_OK) {
        reply[0] = 0x06; /* ACK */
        return 1;
    } else if (result == PARSE_ERROR) {
        reply[0] = 0x15; /* NAK */
        return 1;
    }
    return 0;
}

const protocol_adapter_t adapter_sgp = {
    .name = "SGP",
    .port_offset = 3,
    .parse = parse_sgp,
    .build_reply = reply_sgp
};

/* Bảng tra cứu protocol adapter theo cổng offset 0..3 */
const protocol_adapter_t * const ADAPTERS[4] = {
    &adapter_tbp, /* P + 0: Temperature Binary */
    &adapter_hlp, /* P + 1: Humidity Line */
    &adapter_evp, /* P + 2: Event TLV (Motion, Door) */
    &adapter_sgp  /* P + 3: Smoke/Gas Protocol */
};

/*
 * test_crc.c
 *
 * Kiểm tra tính đúng đắn của thuật toán CRC-16/CCITT-FALSE dùng cho giao thức SGP.
 * Yêu cầu đề bài (Mục 3.4.4):
 *   1. Vector kiểm tra chuẩn: CRC của chuỗi ASCII "123456789" là 0x29B1.
 *   2. Vector ví dụ trong PDF (Mục 3.4.4 & 3.5):
 *      id = 50, 120 ppm, ts = 1767225600 -> CRC = 0xACF5.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "protocol_adapter.h"

int main(void) {
    printf("=== KIỂM THỬ THUẬT TOÁN CRC-16/CCITT-FALSE ===\n");

    /* 1. Kiểm tra với chuỗi ASCII "123456789" */
    const char *test_str = "123456789";
    uint16_t crc1 = crc16_ccitt_false((const uint8_t *)test_str, strlen(test_str));
    printf("[TEST 1] CRC(\"123456789\") = 0x%04X (Kỳ vọng: 0x29B1) -> ", crc1);
    if (crc1 == 0x29B1) {
        printf("PASSED\n");
    } else {
        printf("FAILED (Sai lệch!)\n");
        return EXIT_FAILURE;
    }

    /* 2. Kiểm tra với frame mẫu trong tài liệu đề bài (offset 1..8):
     * id = 50 (0x0032), ppm = 120 (0x0078), ts = 1767225600 (0x6955B900)
     */
    uint8_t sgp_payload[] = { 0x00, 0x32, 0x00, 0x78, 0x69, 0x55, 0xB9, 0x00 };
    uint16_t crc2 = crc16_ccitt_false(sgp_payload, sizeof(sgp_payload));
    printf("[TEST 2] CRC(SGP_Payload_id50) = 0x%04X (Kỳ vọng: 0xACF5) -> ", crc2);
    if (crc2 == 0xACF5) {
        printf("PASSED\n");
    } else {
        printf("FAILED (Sai lệch!)\n");
        return EXIT_FAILURE;
    }

    printf(">>> TẤT CẢ CÁC BÀI KIỂM THỬ CRC-16 ĐÃ ĐẠT TIÊU CHUẨN!\n\n");
    return EXIT_SUCCESS;
}


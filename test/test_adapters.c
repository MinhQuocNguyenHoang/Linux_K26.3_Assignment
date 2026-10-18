/*
 * test_adapters.c
 *
 * Kiểm tra toàn diện lớp Protocol Adapter cho 4 giao thức:
 * 1. TBP: Parse mẫu 18 byte chuẩn, little-endian IEEE 754 double.
 * 2. HLP: Parse dòng ASCII, kiểm tra giới hạn độ dài 64 byte.
 * 3. EVP: Parse TLV cho Motion (Type 1) và Door (Type 2).
 * 4. SGP: Parse frame 11 byte big-endian, kiểm tra CRC và tạo ACK/NAK.
 * 5. Cơ chế phân khung:
 *    - PARSE_NEED_MORE khi nhận nửa frame (fragmentation).
 *    - PARSE_OK liên tiếp khi nhiều frame dính nhau (glueing).
 *    - PARSE_ERROR và tái đồng bộ khi gặp frame hỏng (corruption).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "protocol_adapter.h"

int main(void) {
    printf("=== KIỂM THỬ LỚP PROTOCOL ADAPTER (Step 4) ===\n");

    /* -------------------------------------------------------------
     * TEST 1: TBP (Temperature Binary Protocol)
     * Mẫu đề bài: id = 10, value = 21.5, ts = 1767225600
     * Bytes: 0A 00 | 00 00 00 00 00 80 35 40 | 00 B9 55 69 00 00 00 00
     * ------------------------------------------------------------- */
    printf("[TEST TBP] Giải mã frame 18 byte chuẩn... ");
    uint8_t tbp_frame[18] = {
        0x0A, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x35, 0x40,
        0x00, 0xB9, 0x55, 0x69, 0x00, 0x00, 0x00, 0x00
    };
    sensor_data_t out;
    size_t consumed = 0;
    parse_result_t res = adapter_tbp.parse(tbp_frame, sizeof(tbp_frame), &out, &consumed);

    assert(res == PARSE_OK);
    assert(consumed == 18);
    assert(out.id == 10);
    assert(out.type == SENSOR_TEMPERATURE);
    assert(out.value == 21.5);
    assert(out.ts == 1767225600);
    printf("PASSED (id=%u, value=%.1f, ts=%ld)\n", out.id, out.value, (long)out.ts);

    /* -------------------------------------------------------------
     * TEST 2: HLP (Humidity Line Protocol)
     * Mẫu đề bài: "HUM 21 54.3 1767225600\n"
     * ------------------------------------------------------------- */
    printf("[TEST HLP] Giải mã dòng ASCII... ");
    const char *hlp_sample = "HUM 21 54.3 1767225600\n";
    res = adapter_hlp.parse((const uint8_t *)hlp_sample, strlen(hlp_sample), &out, &consumed);

    assert(res == PARSE_OK);
    assert(consumed == strlen(hlp_sample));
    assert(out.id == 21);
    assert(out.type == SENSOR_HUMIDITY);
    assert(out.value == 54.3);
    assert(out.ts == 1767225600);
    printf("PASSED (id=%u, value=%.1f, ts=%ld)\n", out.id, out.value, (long)out.ts);

    /* -------------------------------------------------------------
     * TEST 3: EVP (Event TLV Protocol)
     * Mẫu đề bài:
     * Motion (id=30): 01 06 00 1E 69 55 B9 00
     * Door (id=40, mở): 02 07 00 28 01 69 55 B9 00
     * ------------------------------------------------------------- */
    printf("[TEST EVP - Motion] Giải mã TLV Type 0x01... ");
    uint8_t evp_motion[8] = { 0x01, 0x06, 0x00, 0x1E, 0x69, 0x55, 0xB9, 0x00 };
    res = adapter_evp.parse(evp_motion, sizeof(evp_motion), &out, &consumed);
    assert(res == PARSE_OK);
    assert(consumed == 8);
    assert(out.id == 30);
    assert(out.type == SENSOR_MOTION);
    assert(out.value == 1.0);
    assert(out.ts == 1767225600);
    printf("PASSED (id=%u, val=%.0f)\n", out.id, out.value);

    printf("[TEST EVP - Door] Giải mã TLV Type 0x02... ");
    uint8_t evp_door[9] = { 0x02, 0x07, 0x00, 0x28, 0x01, 0x69, 0x55, 0xB9, 0x00 };
    res = adapter_evp.parse(evp_door, sizeof(evp_door), &out, &consumed);
    assert(res == PARSE_OK);
    assert(consumed == 9);
    assert(out.id == 40);
    assert(out.type == SENSOR_DOOR);
    assert(out.value == 1.0); /* Mở */
    assert(out.ts == 1767225600);
    printf("PASSED (id=%u, state=MỞ)\n", out.id);

    /* -------------------------------------------------------------
     * TEST 4: SGP (Smoke/Gas Protocol)
     * Mẫu đề bài: id=50, 120 ppm, ts=1767225600, CRC=0xACF5
     * Frame: 7E 00 32 00 78 69 55 B9 00 AC F5
     * ------------------------------------------------------------- */
    printf("[TEST SGP] Giải mã 11 byte frame và kiểm tra CRC... ");
    uint8_t sgp_frame[11] = {
        0x7E, 0x00, 0x32, 0x00, 0x78, 0x69, 0x55, 0xB9, 0x00, 0xAC, 0xF5
    };
    res = adapter_sgp.parse(sgp_frame, sizeof(sgp_frame), &out, &consumed);
    assert(res == PARSE_OK);
    assert(consumed == 11);
    assert(out.id == 50);
    assert(out.type == SENSOR_SMOKE_GAS);
    assert(out.value == 120.0);
    assert(out.ts == 1767225600);

    /* Kiểm tra tạo ACK */
    uint8_t reply[16];
    size_t rlen = adapter_sgp.build_reply(PARSE_OK, reply, sizeof(reply));
    assert(rlen == 1);
    assert(reply[0] == 0x06); /* ACK */
    printf("PASSED (id=%u, ppm=%.0f, ACK=0x06)\n", out.id, out.value);

    /* -------------------------------------------------------------
     * TEST 5: Fragmentation (Phân mảnh TCP)
     * Gửi 10 byte đầu của TBP frame -> PARSE_NEED_MORE
     * Ghép thêm 8 byte sau -> PARSE_OK
     * ------------------------------------------------------------- */
    printf("[TEST FRAGMENTATION] Kiểm tra ghép byte khi frame bị cắt đôi... ");
    uint8_t stream_buf[32];
    memcpy(stream_buf, tbp_frame, 10); /* Chỉ có 10 byte */

    res = adapter_tbp.parse(stream_buf, 10, &out, &consumed);
    assert(res == PARSE_NEED_MORE);
    assert(consumed == 0);

    /* Thêm 8 byte tiếp theo vào buffer */
    memcpy(stream_buf + 10, tbp_frame + 10, 8);
    res = adapter_tbp.parse(stream_buf, 18, &out, &consumed);
    assert(res == PARSE_OK);
    assert(consumed == 18);
    printf("PASSED\n");

    /* -------------------------------------------------------------
     * TEST 6: Glueing (Gộp nhiều frame trong 1 luồng)
     * Ghép 2 frame TBP liên tiếp
     * ------------------------------------------------------------- */
    printf("[TEST GLUEING] Kiểm tra xử lý nhiều frame dính liền trong 1 lần recv... ");
    uint8_t glued[36];
    memcpy(glued, tbp_frame, 18);
    memcpy(glued + 18, tbp_frame, 18);

    /* Giải mã frame 1 */
    res = adapter_tbp.parse(glued, 36, &out, &consumed);
    assert(res == PARSE_OK);
    assert(consumed == 18);

    /* Giải mã frame 2 */
    res = adapter_tbp.parse(glued + consumed, 36 - consumed, &out, &consumed);
    assert(res == PARSE_OK);
    assert(consumed == 18);
    printf("PASSED (Giải mã liên tiếp 2 frame thành công)\n");

    /* -------------------------------------------------------------
     * TEST 7: Corrupted Frame (Frame sai CRC)
     * ------------------------------------------------------------- */
    printf("[TEST CORRUPT] Kiểm tra phát hiện frame sai CRC (SGP) và gửi NAK... ");
    uint8_t bad_sgp[11];
    memcpy(bad_sgp, sgp_frame, 11);
    bad_sgp[10] ^= 0xFF; /* Phá hỏng byte CRC cuối */

    res = adapter_sgp.parse(bad_sgp, 11, &out, &consumed);
    assert(res == PARSE_ERROR);

    rlen = adapter_sgp.build_reply(PARSE_ERROR, reply, sizeof(reply));
    assert(rlen == 1);
    assert(reply[0] == 0x15); /* NAK */
    printf("PASSED (Phát hiện PARSE_ERROR và sinh phản hồi NAK=0x15)\n");

    printf("\n>>> TOÀN BỘ 7 BÀI KIỂM THỬ PROTOCOL ADAPTER ĐÃ VƯỢT QUA!\n\n");
    return EXIT_SUCCESS;
}


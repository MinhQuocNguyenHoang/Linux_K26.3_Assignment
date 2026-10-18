/*
 * sensor_node.c
 *
 * Chương trình mô phỏng Sensor Node client theo Mục 3.7 của đề bài.
 * Kết nối tới Gateway qua TCP theo cổng tương ứng của từng loại cảm biến.
 * Cú pháp:
 *   ./sensor_node <loại> <id> <host> <cổng gốc P> <chu kỳ ms> [tùy chọn]
 *
 * Loại cảm biến:
 *   - temperature : Cổng P (giao thức TBP, 18 byte binary little-endian)
 *   - humidity    : Cổng P+1 (giao thức HLP, ASCII line kết thúc bằng '\n')
 *   - motion      : Cổng P+2 (giao thức EVP, Type 0x01 TLV big-endian)
 *   - door        : Cổng P+2 (giao thức EVP, Type 0x02 TLV big-endian)
 *   - smoke_gas   : Cổng P+3 (giao thức SGP, 11 byte binary big-endian, CRC-16, chờ ACK)
 *
 * Tùy chọn kiểm thử:
 *   --fragment    : Cắt một frame thành nhiều lần send(), nghỉ giữa chừng
 *   --glue        : Gộp nhiều frame vào một lần send() duy nhất
 *   --corrupt     : Thỉnh thoảng gửi frame lỗi (sai magic, sai CRC, sai độ dài)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "protocol_adapter.h"

static volatile sig_atomic_t g_node_stop = 0;

static void node_sig_handler(int sig) {
    (void)sig;
    g_node_stop = 1;
}

/* Lấy chuỗi thời gian hiện tại YYYY-MM-DD HH:MM:SS */
static void get_current_time_str(char *buf, size_t sz) {
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tm_info);
}

/* Gửi dữ liệu có hỗ trợ --fragment */
static ssize_t send_data(int fd, const uint8_t *data, size_t len, int opt_fragment) {
    if (!opt_fragment || len <= 2) {
        return send(fd, data, len, 0);
    }

    /* Chia làm 2 phần gửi cách nhau 50ms */
    size_t part1 = len / 2;
    size_t part2 = len - part1;

    ssize_t s1 = send(fd, data, part1, 0);
    if (s1 <= 0) return s1;

    usleep(50000); /* 50ms */

    ssize_t s2 = send(fd, data + part1, part2, 0);
    if (s2 <= 0) return s2;

    return s1 + s2;
}

int main(int argc, char *argv[]) {
    if (argc < 6) {
        fprintf(stderr, "Cú pháp: %s <loại> <id> <host> <cổng gốc P> <chu kỳ ms> [tùy chọn]\n", argv[0]);
        fprintf(stderr, "Loại:    temperature | humidity | motion | door | smoke_gas\n");
        fprintf(stderr, "Tùy chọn: --fragment | --glue | --corrupt\n");
        return EXIT_FAILURE;
    }

    const char *type_str = argv[1];
    sensor_id_t sensor_id = (sensor_id_t)atoi(argv[2]);
    const char *host = argv[3];
    int base_port = atoi(argv[4]);
    int period_ms = atoi(argv[5]);

    int opt_fragment = 0;
    int opt_glue = 0;
    int opt_corrupt = 0;

    for (int i = 6; i < argc; i++) {
        if (strcmp(argv[i], "--fragment") == 0) opt_fragment = 1;
        else if (strcmp(argv[i], "--glue") == 0) opt_glue = 1;
        else if (strcmp(argv[i], "--corrupt") == 0) opt_corrupt = 1;
    }

    /* Xác định loại sensor và cổng kết nối tương ứng */
    sensor_type_t stype = 0;
    int port_offset = 0;

    if (strcmp(type_str, "temperature") == 0) {
        stype = SENSOR_TEMPERATURE;
        port_offset = 0;
    } else if (strcmp(type_str, "humidity") == 0) {
        stype = SENSOR_HUMIDITY;
        port_offset = 1;
    } else if (strcmp(type_str, "motion") == 0) {
        stype = SENSOR_MOTION;
        port_offset = 2;
    } else if (strcmp(type_str, "door") == 0) {
        stype = SENSOR_DOOR;
        port_offset = 2;
    } else if (strcmp(type_str, "smoke_gas") == 0) {
        stype = SENSOR_SMOKE_GAS;
        port_offset = 3;
    } else {
        fprintf(stderr, "Lỗi: Loại sensor '%s' không hợp lệ.\n", type_str);
        return EXIT_FAILURE;
    }

    int target_port = base_port + port_offset;

    /* Thiết lập bắt tín hiệu Ctrl+C */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = node_sig_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Khởi tạo socket TCP và kết nối tới Gateway */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket that bai");
        return EXIT_FAILURE;
    }

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(target_port);

    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        char tbuf[32];
        get_current_time_str(tbuf, sizeof(tbuf));
        printf("%s sensor %u %s connect failed (Invalid address)\n", tbuf, sensor_id, type_str);
        close(sockfd);
        return EXIT_FAILURE;
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        char tbuf[32];
        get_current_time_str(tbuf, sizeof(tbuf));
        printf("%s sensor %u %s connect failed (%s)\n", tbuf, sensor_id, type_str, strerror(errno));
        close(sockfd);
        return EXIT_FAILURE;
    }

    /* In ra stdout khi kết nối thành công theo Mục 3.8 */
    char tbuf[32];
    get_current_time_str(tbuf, sizeof(tbuf));
    printf("%s sensor %u %s connected to %s:%d\n", tbuf, sensor_id, type_str, host, target_port);
    fflush(stdout);

    srand((unsigned int)(time(NULL) ^ (sensor_id << 4)));

    /* Trạng thái mô phỏng cho từng loại */
    double temp_val = 18.5;
    double hum_val = 55.0;
    uint8_t door_state = 0;
    int frame_counter = 0;

    while (!g_node_stop) {
        frame_counter++;
        time_t now = time(NULL);
        uint8_t frame_buf[256];
        size_t frame_len = 0;

        int should_corrupt = (opt_corrupt && (frame_counter % 5 == 0));

        /* Đóng gói frame theo từng giao thức */
        if (stype == SENSOR_TEMPERATURE) {
            /* Biến thiên nhiệt độ ngẫu nhiên từ 14°C đến 25°C */
            double delta = ((rand() % 21) - 10) * 0.2; /* -2.0 đến +2.0 */
            temp_val += delta;
            if (temp_val < 12.0) temp_val = 14.0;
            if (temp_val > 28.0) temp_val = 24.0;

            uint16_t id_le = sensor_id;
            int64_t ts_le = (int64_t)now;

            memcpy(frame_buf + 0, &id_le, 2);
            memcpy(frame_buf + 2, &temp_val, 8);
            memcpy(frame_buf + 10, &ts_le, 8);
            frame_len = 18;

            if (should_corrupt) {
                double invalid_temp = 999.0;
                memcpy(frame_buf + 2, &invalid_temp, 8);
            }
        }
        else if (stype == SENSOR_HUMIDITY) {
            double delta = ((rand() % 21) - 10) * 1.0;
            hum_val += delta;
            if (hum_val < 25.0) hum_val = 28.0;
            if (hum_val > 80.0) hum_val = 75.0;

            if (should_corrupt) {
                frame_len = snprintf((char *)frame_buf, sizeof(frame_buf),
                                     "INVALID_FRAME %u %.1f %ld\n", sensor_id, hum_val, (long)now);
            } else {
                frame_len = snprintf((char *)frame_buf, sizeof(frame_buf),
                                     "HUM %u %.1f %ld\n", sensor_id, hum_val, (long)now);
            }
        }
        else if (stype == SENSOR_MOTION) {
            frame_buf[0] = 0x01; /* Motion Type */
            frame_buf[1] = 0x06; /* Length */
            frame_buf[2] = (uint8_t)(sensor_id >> 8);
            frame_buf[3] = (uint8_t)(sensor_id & 0xFF);
            uint32_t ts_be = (uint32_t)now;
            frame_buf[4] = (uint8_t)(ts_be >> 24);
            frame_buf[5] = (uint8_t)(ts_be >> 16);
            frame_buf[6] = (uint8_t)(ts_be >> 8);
            frame_buf[7] = (uint8_t)(ts_be & 0xFF);
            frame_len = 8;

            if (should_corrupt) {
                frame_buf[0] = 0x99; /* Invalid Type */
            }
        }
        else if (stype == SENSOR_DOOR) {
            door_state = 1 - door_state; /* Đổi trạng thái đóng/mở */
            frame_buf[0] = 0x02; /* Door Type */
            frame_buf[1] = 0x07; /* Length */
            frame_buf[2] = (uint8_t)(sensor_id >> 8);
            frame_buf[3] = (uint8_t)(sensor_id & 0xFF);
            frame_buf[4] = door_state;
            uint32_t ts_be = (uint32_t)now;
            frame_buf[5] = (uint8_t)(ts_be >> 24);
            frame_buf[6] = (uint8_t)(ts_be >> 16);
            frame_buf[7] = (uint8_t)(ts_be >> 8);
            frame_buf[8] = (uint8_t)(ts_be & 0xFF);
            frame_len = 9;

            if (should_corrupt) {
                frame_buf[1] = 0x02; /* Bad length */
            }
        }
        else if (stype == SENSOR_SMOKE_GAS) {
            uint16_t ppm = 100 + (rand() % 350); /* Có lúc vượt ngưỡng 300 ppm */
            frame_buf[0] = 0x7E; /* Magic */
            frame_buf[1] = (uint8_t)(sensor_id >> 8);
            frame_buf[2] = (uint8_t)(sensor_id & 0xFF);
            frame_buf[3] = (uint8_t)(ppm >> 8);
            frame_buf[4] = (uint8_t)(ppm & 0xFF);
            uint32_t ts_be = (uint32_t)now;
            frame_buf[5] = (uint8_t)(ts_be >> 24);
            frame_buf[6] = (uint8_t)(ts_be >> 16);
            frame_buf[7] = (uint8_t)(ts_be >> 8);
            frame_buf[8] = (uint8_t)(ts_be & 0xFF);

            /* Tính CRC của byte offset 1..8 */
            uint16_t crc = crc16_ccitt_false(&frame_buf[1], 8);
            if (should_corrupt) {
                crc ^= 0xFFFF; /* Làm sai CRC */
            }
            frame_buf[9] = (uint8_t)(crc >> 8);
            frame_buf[10] = (uint8_t)(crc & 0xFF);
            frame_len = 11;
        }

        /* Xử lý tùy chọn --glue (gộp 2 frame vào 1 lần send) */
        if (opt_glue && frame_len * 2 < sizeof(frame_buf)) {
            memcpy(frame_buf + frame_len, frame_buf, frame_len);
            frame_len *= 2;
        }

        /* Gửi frame qua socket */
        if (stype == SENSOR_SMOKE_GAS) {
            /* SGP: Cơ chế Stop-and-Wait với ACK/NAK và thử lại tối đa 3 lần */
            int ack_received = 0;
            int retries = 0;

            while (!ack_received && retries < 3 && !g_node_stop) {
                send_data(sockfd, frame_buf, frame_len, opt_fragment);

                /* Đặt timeout nhận ACK là 1 giây */
                struct timeval tv;
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));

                uint8_t ack_byte = 0;
                ssize_t r = recv(sockfd, &ack_byte, 1, 0);

                if (r == 1 && ack_byte == 0x06) {
                    ack_received = 1;
                } else {
                    retries++;
                    usleep(100000); /* Chờ 100ms trước khi gửi lại */
                }
            }
        } else {
            /* Các giao thức khác: gửi trực tiếp */
            ssize_t s = send_data(sockfd, frame_buf, frame_len, opt_fragment);
            if (s <= 0) {
                break;
            }
        }

        /* Nghỉ theo chu kỳ (có thể biến thiên với motion/door) */
        int sleep_time = period_ms;
        if (stype == SENSOR_MOTION || stype == SENSOR_DOOR) {
            /* Phân phối ngẫu nhiên xung quanh period_ms */
            int jitter = (rand() % (period_ms / 2 + 1)) - (period_ms / 4);
            sleep_time += jitter;
            if (sleep_time < 200) sleep_time = 200;
        }

        usleep((useconds_t)sleep_time * 1000);
    }

    close(sockfd);
    return EXIT_SUCCESS;
}


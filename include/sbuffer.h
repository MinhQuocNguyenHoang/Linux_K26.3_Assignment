/*
 * sbuffer.h
 *
 * Hàng đợi dùng chung (Shared Buffer) duy nhất giữa ba luồng:
 * Connection Manager (ghi), Data Manager (đọc), Storage Manager (đọc).
 * Khung cấu trúc cung cấp sẵn theo Mục 3.9 và Mục 3.10 của đề bài.
 */

#ifndef SBUFFER_H
#define SBUFFER_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>
#include "sensor_types.h"

/* Nút trong danh sách liên kết đơn sbuffer */
typedef struct sbuffer_node {
    sensor_data_t data;
    bool read_by_data;    /* Data manager đã đọc */
    bool read_by_storage; /* Storage manager đã đọc */
    struct sbuffer_node *next;
} sbuffer_node_t;

/* Cấu trúc quản lý sbuffer */
typedef struct {
    sbuffer_node_t *head, *tail;
    size_t size;
    bool shutdown;            /* Báo hiệu kết thúc hệ thống */
    pthread_mutex_t lock;     /* Khóa bảo vệ cấu trúc hàng đợi */
    pthread_cond_t not_empty; /* Condition variable đánh thức thread đọc */
} sbuffer_t;

/* Khởi tạo sbuffer và các đối tượng đồng bộ */
int sbuffer_init(sbuffer_t *sbuf);

/* Thêm một phần tử mới vào cuối sbuffer (Connection Manager gọi) */
int sbuffer_insert(sbuffer_t *sbuf, const sensor_data_t *data);

/*
 * Đọc dành riêng cho Data Manager:
 * Tìm phần tử đầu tiên mà Data Manager chưa đọc.
 * Đánh dấu read_by_data = true.
 * Nếu phần tử đó cũng đã được Storage Manager đọc, loại bỏ và giải phóng bộ nhớ.
 * Trả về:
 *   1 : Đọc thành công một phần tử mới vào *out.
 *   0 : Hàng đợi không còn phần tử mới và gateway đã phát tín hiệu shutdown.
 *  -1 : Lỗi đồng bộ.
 */
int sbuffer_read_data(sbuffer_t *sbuf, sensor_data_t *out);

/*
 * Đọc dành riêng cho Storage Manager:
 * Tìm phần tử đầu tiên mà Storage Manager chưa đọc.
 * Đánh dấu read_by_storage = true.
 * Nếu phần tử đó cũng đã được Data Manager đọc, loại bỏ và giải phóng bộ nhớ.
 * Trả về:
 *   1 : Đọc thành công một phần tử mới vào *out.
 *   0 : Hàng đợi không còn phần tử mới và gateway đã phát tín hiệu shutdown.
 *  -1 : Lỗi đồng bộ.
 */
int sbuffer_read_storage(sbuffer_t *sbuf, sensor_data_t *out);

/* Phát tín hiệu dừng sbuffer, đánh thức tất cả các thread đang chờ */
void sbuffer_shutdown(sbuffer_t *sbuf);

/* Thu dọn tài nguyên, hủy mutex và condvar, giải phóng mọi node còn lại */
void sbuffer_cleanup(sbuffer_t *sbuf);

/* Lấy số lượng phần tử hiện đang có trong sbuffer */
size_t sbuffer_get_size(sbuffer_t *sbuf);

#endif /* SBUFFER_H */


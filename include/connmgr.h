/*
 * connmgr.h
 *
 * Quản lý kết nối TCP từ các sensor node (Connection Manager Thread).
 * Lắng nghe trên 4 cổng liên tiếp (P..P+3) và hỗ trợ đa kết nối đồng thời.
 * Cung cấp sẵn theo Mục 3.10 của đề bài.
 */

#ifndef CONNMGR_H
#define CONNMGR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "protocol_adapter.h"

#define RX_BUF_SIZE 256
#define MAX_CONN 64

/* Cấu trúc quản lý một kết nối TCP riêng biệt từ một sensor node */
typedef struct {
    int fd;                            /* File descriptor socket */
    const protocol_adapter_t *adapter; /* Con trỏ tới protocol adapter tương ứng với cổng */
    uint8_t rx[RX_BUF_SIZE];           /* Bộ đệm nhận riêng của kết nối */
    size_t rx_len;                     /* Số byte hiện có trong bộ đệm nhận */
    bool identified;                   /* Cờ đánh dấu đã nhận diện sensor ID qua frame đầu */
    sensor_id_t sensor_id;             /* ID của sensor sau khi đã nhận diện */
} connection_t;

/* Cấu trúc quản lý toàn bộ Connection Manager */
typedef struct {
    int listen_fd[4];                  /* Socket lắng nghe của 4 cổng P .. P+3 */
    connection_t conns[MAX_CONN];      /* Danh sách kết nối đang hoạt động */
    size_t n_conns;                    /* Số lượng kết nối hiện tại */
} connmgr_t;

/* Khởi tạo socket lắng nghe trên 4 cổng (cổng gốc base_port đến base_port+3) */
int connmgr_init(connmgr_t *cm, uint16_t base_port);

/* Thu dọn tất cả socket kết nối và socket lắng nghe */
void connmgr_cleanup(connmgr_t *cm);

/* Hàm thực thi của Connection Manager Thread */
void *connmgr_thread(void *arg);

#endif /* CONNMGR_H */


/*
 * storagemgr.h
 *
 * Quản lý lưu trữ dữ liệu vào SQLite (Storage Manager Thread).
 * Hỗ trợ tạo bảng, ghi bản ghi và tự động thử lại (retry) khi mất kết nối.
 * Cung cấp sẵn theo Mục 3.10 của đề bài.
 */

#ifndef STORAGEMGR_H
#define STORAGEMGR_H

#include <sqlite3.h>
#include "sensor_types.h"
#include "shared_status.h"

/* Cấu trúc quản lý lưu trữ SQLite */
typedef struct {
    sqlite3 *db;           /* Con trỏ kết nối SQLite */
    sqlite3_stmt *insert;  /* Prepared statement thực hiện INSERT */
    sql_state_t state;     /* Trạng thái kết nối SQL */
    int attempts;          /* Số lần đã thử kết nối lại */
    const char *db_path;   /* Đường dẫn tới file cơ sở dữ liệu */
} storagemgr_t;

/* Khởi tạo cơ sở dữ liệu, bật chế độ WAL và tạo bảng SensorData nếu chưa có */
int storagemgr_init(storagemgr_t *sm, const char *db_path);

/* Thử kết nối lại với SQLite nếu gặp sự cố */
int storagemgr_reconnect(storagemgr_t *sm);

/* Thu dọn tài nguyên và đóng cơ sở dữ liệu */
void storagemgr_cleanup(storagemgr_t *sm);

/* Hàm thực thi của Storage Manager Thread */
void *storagemgr_thread(void *arg);

#endif /* STORAGEMGR_H */


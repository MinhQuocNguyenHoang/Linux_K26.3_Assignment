/*
 * shared_status.h
 *
 * Định nghĩa cấu trúc vùng nhớ chia sẻ POSIX Shared Memory
 * giữa Main Process (3 luồng ghi) và Web Process (tiến trình đọc).
 * Bảo vệ bằng Mutex liên tiến trình (PTHREAD_PROCESS_SHARED) có thuộc tính ROBUST.
 * Cung cấp sẵn theo Mục 3.10 và Mục 3.11 của đề bài.
 */

#ifndef SHARED_STATUS_H
#define SHARED_STATUS_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include "sensor_types.h"

#define MAX_SENSORS 64

/* Trạng thái hoạt động của cảm biến */
typedef enum {
    STATE_COLLECTING, /* Đang thu thập mẫu ban đầu (< RUN_AVG_LENGTH) */
    STATE_NORMAL,     /* Bình thường (trong ngưỡng an toàn) */
    STATE_ALERT       /* Cảnh báo (vượt ngưỡng hoặc phát hiện sự kiện bất thường) */
} sensor_state_t;

/* Trạng thái kết nối cơ sở dữ liệu SQLite */
typedef enum {
    SQL_CONNECTED, /* Đang kết nối bình thường */
    SQL_LOST,      /* Mất kết nối, đang trong quá trình thử kết nối lại */
    SQL_DOWN       /* Mất kết nối hoàn toàn sau 3 lần thử thất bại */
} sql_state_t;

/* Trạng thái tức thời của một cảm biến trong shared memory */
typedef struct {
    sensor_id_t    id;          /* ID của sensor */
    uint16_t       room;        /* Số phòng lắp đặt */
    sensor_type_t  type;        /* Loại cảm biến */
    double         last_value;  /* Giá trị đo mới nhất */
    double         running_avg; /* Giá trị trung bình trượt (nếu có) */
    bool           has_avg;     /* Có hỗ trợ trung bình trượt hay không */
    sensor_state_t state;       /* Trạng thái cảm biến */
    bool           connected;   /* Trạng thái kết nối TCP hiện tại */
    time_t         last_update; /* Thời điểm cập nhật cuối cùng (giây UTC) */
} sensor_status_t;

/* Bảng trạng thái toàn cục của Gateway trong Shared Memory */
typedef struct {
    pthread_mutex_t lock;       /* Mutex liên tiến trình: PTHREAD_PROCESS_SHARED + ROBUST */
    uint64_t        received;   /* Tổng số phép đo đã nhận */
    uint64_t        processed;  /* Tổng số phép đo đã xử lý */
    uint64_t        stored;     /* Tổng số phép đo đã lưu vào SQLite */
    uint32_t        sbuffer_size; /* Kích thước hiện tại của sbuffer */
    sql_state_t     sql_state;  /* Trạng thái kết nối SQLite */
    uint8_t         sql_retries;/* Số lần đã thử kết nối lại SQL */
    time_t          start_time; /* Thời điểm khởi động hệ thống */
    uint16_t        sensor_count; /* Số lượng sensor đã đăng ký */
    sensor_status_t sensors[MAX_SENSORS]; /* Danh sách trạng thái sensor */
} gateway_status_t;

#define SHM_DEFAULT_NAME "/gatewayStatus"

/* Khởi tạo vùng shared memory và robust mutex (Main process gọi trước khi fork) */
gateway_status_t *shared_status_create(const char *name);

/* Mở vùng shared memory sẵn có (Web process gọi) */
gateway_status_t *shared_status_open(const char *name);

/* Đóng ánh xạ shared memory */
void shared_status_detach(gateway_status_t *status);

/* Xóa bỏ hoàn toàn shared memory và hủy mutex (Main process gọi khi kết thúc) */
void shared_status_destroy(const char *name, gateway_status_t *status);

/* Thao tác khóa/mở khóa an toàn với xử lý EOWNERDEAD */
int shared_status_lock(gateway_status_t *status);
int shared_status_unlock(gateway_status_t *status);

#endif /* SHARED_STATUS_H */


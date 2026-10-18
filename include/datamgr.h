/*
 * datamgr.h
 *
 * Xử lý dữ liệu cảm biến (Data Manager Thread).
 * Nạp danh mục phòng-sensor, tính trung bình trượt, kiểm tra ngưỡng và phát hiện bất thường.
 * Cung cấp sẵn theo Mục 3.10 của đề bài.
 */

#ifndef DATAMGR_H
#define DATAMGR_H

#include <stdint.h>
#include <stddef.h>
#include "sensor_types.h"
#include "sensor_config.h"
#include "shared_status.h"

/* Thông tin cấu hình phòng cho một sensor từ file cấu hình */
typedef struct {
    uint16_t room;      /* Số phòng */
    sensor_id_t id;     /* ID sensor */
    sensor_type_t type; /* Loại sensor */
} sensor_info_t;

/* Trạng thái xử lý dữ liệu của từng sensor trong Data Manager */
typedef struct {
    sensor_info_t info;
    double window[RUN_AVG_LENGTH]; /* Vòng đệm giá trị trung bình trượt */
    size_t count, next;            /* Số mẫu hiện có và chỉ số ghi tiếp theo */
    double running_avg;            /* Giá trị trung bình trượt hiện tại */
    sensor_state_t state;          /* Trạng thái hiện tại của sensor */
} datamgr_sensor_t;

/* Cấu trúc ngữ cảnh Data Manager */
typedef struct {
    datamgr_sensor_t sensors[MAX_SENSORS];
    size_t n;
} datamgr_t;

/* Kiểu hàm xử lý chuyên biệt cho từng loại sensor (Handler Table) */
typedef void (*sensor_handler_t)(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *data);

/* Nạp danh mục phòng - sensor từ file ánh xạ (chỉ đọc 1 lần khi khởi động) */
int datamgr_load_map(datamgr_t *dm, const char *map_file_path);

/* Tìm sensor trong bảng cấu hình theo id */
datamgr_sensor_t *datamgr_find_sensor(datamgr_t *dm, sensor_id_t id);

/* Hàm thực thi của Data Manager Thread */
void *datamgr_thread(void *arg);

#endif /* DATAMGR_H */


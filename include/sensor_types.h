/*
 * sensor_types.h
 *
 * Định nghĩa các kiểu dữ liệu cảm biến chuẩn hóa dùng chung toàn hệ thống.
 * Cung cấp sẵn theo đặc tả Mục 3.2 của đề bài.
 */

#ifndef SENSOR_TYPES_H
#define SENSOR_TYPES_H

#include <stdint.h>
#include <time.h>

/* Kiểu định danh sensor: duy nhất trong toàn hệ thống */
typedef uint16_t sensor_id_t;

/* Các loại cảm biến được hỗ trợ */
typedef enum {
    SENSOR_TEMPERATURE = 1, /* Cảm biến nhiệt độ (°C) */
    SENSOR_HUMIDITY    = 2, /* Cảm biến độ ẩm (%RH) */
    SENSOR_MOTION      = 3, /* Cảm biến chuyển động (1 = phát hiện) */
    SENSOR_DOOR        = 4, /* Cảm biến cửa (0 = đóng, 1 = mở) */
    SENSOR_SMOKE_GAS   = 5  /* Cảm biến khói / gas (ppm) */
} sensor_type_t;

/* Cấu trúc dữ liệu chuẩn hóa của mỗi phép đo */
typedef struct {
    sensor_id_t   id;    /* ID duy nhất của sensor */
    sensor_type_t type;  /* Loại cảm biến */
    double        value; /* Giá trị theo đơn vị tương ứng */
    time_t        ts;    /* Thời điểm đo (giây UTC) */
} sensor_data_t;

#endif /* SENSOR_TYPES_H */


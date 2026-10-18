/*
 * sensor_config.h
 *
 * Định nghĩa các tham số cấu hình, độ dài cửa sổ trượt trung bình
 * và các ngưỡng cảnh báo mặc định theo Mục 3.3 của đề bài.
 */

#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

/* Số phép đo dùng để tính giá trị trung bình trượt (Running Average) */
#define RUN_AVG_LENGTH 5

/* Ngưỡng nhiệt độ (°C) */
#define TEMP_MIN_C 15.0  /* Dưới ngưỡng này: cảnh báo Quá lạnh */
#define TEMP_MAX_C 20.0  /* Trên ngưỡng này: cảnh báo Quá nóng */

/* Ngưỡng độ ẩm (%RH) */
#define HUMIDITY_MIN 30.0 /* Dưới ngưỡng này: cảnh báo Quá khô */
#define HUMIDITY_MAX 70.0 /* Trên ngưỡng này: cảnh báo Quá ẩm */

/* Ngưỡng nồng độ khí gas / khói (ppm) - so sánh tức thời */
#define GAS_ALARM_PPM 300.0 /* Lớn hơn hoặc bằng ngưỡng này: Báo động khói/gas */

#endif /* SENSOR_CONFIG_H */


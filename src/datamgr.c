/*
 * datamgr.c
 *
 * Hiện thực Data Manager Thread.
 * Chức năng:
 * 1. Nạp và tra cứu file ánh xạ room-sensor để xác thực sensor ID và loại sensor hợp lệ.
 * 2. Tính giá trị trung bình trượt (Running Average) 5 phép đo cho Nhiệt độ và Độ ẩm.
 * 3. So sánh ngưỡng và phát hiện sự kiện bất thường (cửa, chuyển động, khói/gas).
 * 4. Ghi log cảnh báo khi chuyển trạng thái (không lặp lại từng phép đo).
 * 5. Cập nhật bảng trạng thái thời gian thực trong Shared Memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "datamgr.h"
#include "gateway.h"
#include "log_process.h"
#include "shared_status.h"

int datamgr_load_map(datamgr_t *dm, const char *map_file_path) {
    if (!dm || !map_file_path) return -1;

    FILE *fp = fopen(map_file_path, "r");
    if (!fp) {
        perror("[DATAMGR] Khong the mo file room-sensor map");
        return -1;
    }

    dm->n = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp)) {
        /* Bỏ qua khoảng trắng đầu dòng */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        /* Bỏ qua dòng chú thích '#' hoặc dòng trống */
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
            continue;
        }

        uint16_t room = 0;
        unsigned int id = 0;
        char type_str[64] = {0};

        if (sscanf(p, "%hu %u %63s", &room, &id, type_str) == 3) {
            sensor_type_t stype = 0;
            if (strcmp(type_str, "temperature") == 0) {
                stype = SENSOR_TEMPERATURE;
            } else if (strcmp(type_str, "humidity") == 0) {
                stype = SENSOR_HUMIDITY;
            } else if (strcmp(type_str, "motion") == 0) {
                stype = SENSOR_MOTION;
            } else if (strcmp(type_str, "door") == 0) {
                stype = SENSOR_DOOR;
            } else if (strcmp(type_str, "smoke_gas") == 0) {
                stype = SENSOR_SMOKE_GAS;
            } else {
                continue; /* Loại không hợp lệ */
            }

            if (dm->n < MAX_SENSORS) {
                datamgr_sensor_t *ds = &dm->sensors[dm->n];
                memset(ds, 0, sizeof(datamgr_sensor_t));
                ds->info.room = room;
                ds->info.id = (sensor_id_t)id;
                ds->info.type = stype;
                ds->state = STATE_COLLECTING;
                ds->count = 0;
                ds->next = 0;
                ds->running_avg = 0.0;
                dm->n++;
            }
        }
    }

    fclose(fp);
#ifdef DEBUG
    printf("[DATAMGR] Da nap %zu sensor hop le tu file %s\n", dm->n, map_file_path);
#endif
    return 0;
}

datamgr_sensor_t *datamgr_find_sensor(datamgr_t *dm, sensor_id_t id) {
    if (!dm) return NULL;
    for (size_t i = 0; i < dm->n; i++) {
        if (dm->sensors[i].info.id == id) {
            return &dm->sensors[i];
        }
    }
    return NULL;
}

/* Handler xử lý cảm biến Nhiệt độ */
static void handle_temperature(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *data) {
    (void)dm;

    /* Cập nhật vòng đệm cửa sổ trượt */
    s->window[s->next] = data->value;
    s->next = (s->next + 1) % RUN_AVG_LENGTH;
    if (s->count < RUN_AVG_LENGTH) {
        s->count++;
    }

    /* Tính giá trị trung bình trượt */
    double sum = 0.0;
    for (size_t i = 0; i < s->count; i++) {
        sum += s->window[i];
    }
    s->running_avg = sum / (double)s->count;

    /* Chỉ so sánh ngưỡng khi đã thu thập đủ 5 mẫu */
    if (s->count >= RUN_AVG_LENGTH) {
        sensor_state_t prev_state = s->state;

        if (s->running_avg < TEMP_MIN_C) {
            s->state = STATE_ALERT;
            if (prev_state != STATE_ALERT) {
                log_event("DATA", "The sensor node with %u reports it's too cold (running avg temperature = %.1f)",
                          (unsigned int)s->info.id, s->running_avg);
            }
        } else if (s->running_avg > TEMP_MAX_C) {
            s->state = STATE_ALERT;
            if (prev_state != STATE_ALERT) {
                log_event("DATA", "The sensor node with %u reports it's too hot (running avg temperature = %.1f)",
                          (unsigned int)s->info.id, s->running_avg);
            }
        } else {
            s->state = STATE_NORMAL;
        }
    } else {
        s->state = STATE_COLLECTING;
    }
}

/* Handler xử lý cảm biến Độ ẩm */
static void handle_humidity(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *data) {
    (void)dm;

    s->window[s->next] = data->value;
    s->next = (s->next + 1) % RUN_AVG_LENGTH;
    if (s->count < RUN_AVG_LENGTH) {
        s->count++;
    }

    double sum = 0.0;
    for (size_t i = 0; i < s->count; i++) {
        sum += s->window[i];
    }
    s->running_avg = sum / (double)s->count;

    if (s->count >= RUN_AVG_LENGTH) {
        sensor_state_t prev_state = s->state;

        if (s->running_avg < HUMIDITY_MIN) {
            s->state = STATE_ALERT;
            if (prev_state != STATE_ALERT) {
                log_event("DATA", "The sensor node with %u reports it's too dry (running avg humidity = %.1f)",
                          (unsigned int)s->info.id, s->running_avg);
            }
        } else if (s->running_avg > HUMIDITY_MAX) {
            s->state = STATE_ALERT;
            if (prev_state != STATE_ALERT) {
                log_event("DATA", "The sensor node with %u reports it's too humid (running avg humidity = %.1f)",
                          (unsigned int)s->info.id, s->running_avg);
            }
        } else {
            s->state = STATE_NORMAL;
        }
    } else {
        s->state = STATE_COLLECTING;
    }
}

/* Handler xử lý cảm biến Chuyển động */
static void handle_motion(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *data) {
    (void)dm;
    (void)data;

    s->state = STATE_ALERT;
    log_event("DATA", "The sensor node with %u detected motion", (unsigned int)s->info.id);
}

/* Handler xử lý cảm biến Cửa */
static void handle_door(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *data) {
    (void)dm;

    int is_open = (data->value >= 0.5);
    s->state = is_open ? STATE_ALERT : STATE_NORMAL;

    if (is_open) {
        log_event("DATA", "The sensor node with %u reports the door is open", (unsigned int)s->info.id);
    } else {
        log_event("DATA", "The sensor node with %u reports the door is closed", (unsigned int)s->info.id);
    }
}

/* Handler xử lý cảm biến Khói / Gas (so sánh tức thời) */
static void handle_smoke_gas(datamgr_t *dm, datamgr_sensor_t *s, const sensor_data_t *data) {
    (void)dm;

    if (data->value >= GAS_ALARM_PPM) {
        s->state = STATE_ALERT;
        log_event("DATA", "The sensor node with %u reports a smoke/gas alarm (concentration = %.1f ppm)",
                  (unsigned int)s->info.id, data->value);
    } else {
        s->state = STATE_NORMAL;
    }
}

/* Bảng handler ánh xạ theo sensor_type_t */
static sensor_handler_t const HANDLERS[6] = {
    NULL,
    handle_temperature, /* 1: SENSOR_TEMPERATURE */
    handle_humidity,    /* 2: SENSOR_HUMIDITY */
    handle_motion,      /* 3: SENSOR_MOTION */
    handle_door,        /* 4: SENSOR_DOOR */
    handle_smoke_gas    /* 5: SENSOR_SMOKE_GAS */
};

/* Cập nhật thông tin vào bảng trạng thái Shared Memory */
static void update_shared_memory(gateway_t *gw, datamgr_sensor_t *s, const sensor_data_t *data) {
    if (!gw->status) return;

    if (shared_status_lock(gw->status) == 0) {
        gw->status->processed++;
        gw->status->sbuffer_size = (uint32_t)sbuffer_get_size(&gw->sbuf);

        /* Tìm sensor trong danh sách shared status */
        int found_idx = -1;
        for (uint16_t i = 0; i < gw->status->sensor_count; i++) {
            if (gw->status->sensors[i].id == s->info.id) {
                found_idx = (int)i;
                break;
            }
        }

        /* Thêm mới nếu chưa có */
        if (found_idx < 0 && gw->status->sensor_count < MAX_SENSORS) {
            found_idx = (int)gw->status->sensor_count;
            gw->status->sensor_count++;
        }

        if (found_idx >= 0) {
            sensor_status_t *ss = &gw->status->sensors[found_idx];
            ss->id = s->info.id;
            ss->room = s->info.room;
            ss->type = s->info.type;
            ss->last_value = data->value;
            ss->running_avg = s->running_avg;
            ss->has_avg = (s->info.type == SENSOR_TEMPERATURE || s->info.type == SENSOR_HUMIDITY);
            ss->state = s->state;
            ss->connected = true;
            ss->last_update = data->ts;
        }

        shared_status_unlock(gw->status);
    }
}

void *datamgr_thread(void *arg) {
    gateway_t *gw = (gateway_t *)arg;
    datamgr_t *dm = &gw->data;

#ifdef DEBUG
    printf("[DATAMGR] Data Manager Thread da khoi dong.\n");
#endif

    while (!gw->stop) {
        sensor_data_t data;
        int ret = sbuffer_read_data(&gw->sbuf, &data);

        if (ret == 1) {
#ifdef DEBUG
            printf("[DATAMGR-DEBUG] Doc sbuffer: ID=%u, Type=%d, Value=%.2f, Ts=%ld\n",
                   data.id, data.type, data.value, (long)data.ts);
#endif
            /* Xác thực sensor ID và kiểm tra loại cảm biến */
            datamgr_sensor_t *s = datamgr_find_sensor(dm, data.id);
            if (!s || s->info.type != data.type) {
                log_event("DATA", "Received sensor data with invalid sensor node ID %u",
                          (unsigned int)data.id);

                if (gw->status && shared_status_lock(gw->status) == 0) {
                    gw->status->processed++;
                    gw->status->sbuffer_size = (uint32_t)sbuffer_get_size(&gw->sbuf);
                    shared_status_unlock(gw->status);
                }
                continue;
            }

            /* Gọi handler chuyên biệt tương ứng */
            if (data.type >= 1 && data.type <= 5 && HANDLERS[data.type]) {
                HANDLERS[data.type](dm, s, &data);
            }

            /* Đưa dữ liệu trạng thái mới vào Shared Memory */
            update_shared_memory(gw, s, &data);
        }
        else if (ret == 0) {
            /* Shutdown và không còn dữ liệu trong sbuffer */
            break;
        }
    }

#ifdef DEBUG
    printf("[DATAMGR] Data Manager Thread dang dung...\n");
#endif
    return NULL;
}


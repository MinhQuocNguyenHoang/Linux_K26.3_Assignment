/*
 * storagemgr.c
 *
 * Hiện thực Storage Manager Thread.
 * Chức năng:
 * 1. Mở và duy trì kết nối tới SQLite (file gateway.db), bật chế độ WAL.
 * 2. Tự động khởi tạo bảng SensorData nếu chưa có.
 * 3. Ghi dữ liệu cảm biến đã chuẩn hóa từ sbuffer vào bảng cơ sở dữ liệu.
 * 4. Xử lý kịch bản mất kết nối SQL: giữ dữ liệu trong sbuffer, thử lại tối đa 3 lần.
 *    Nếu cả 3 lần thất bại, ghi log và phát tín hiệu dừng gateway có kiểm soát.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "storagemgr.h"
#include "gateway.h"
#include "log_process.h"
#include "shared_status.h"

int storagemgr_init(storagemgr_t *sm, const char *db_path) {
    if (!sm || !db_path) return -1;

    memset(sm, 0, sizeof(storagemgr_t));
    sm->db_path = db_path;
    sm->state = SQL_LOST;
    sm->attempts = 0;

    int rc = sqlite3_open(db_path, &sm->db);
    if (rc != SQLITE_OK) {
        log_event("STOR", "Unable to connect to SQL server.");
        sm->state = SQL_DOWN;
        return -1;
    }

    /* Bật chế độ WAL (Write-Ahead Logging) để Web Process (reader) không bị khóa bởi Storage Manager (writer) */
    sqlite3_exec(sm->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_busy_timeout(sm->db, 2000);

    log_event("STOR", "Connection to SQL server established.");
    sm->state = SQL_CONNECTED;
    sm->attempts = 0;

    /* Kiểm tra xem bảng đã tồn tại hay chưa */
    sqlite3_stmt *check_stmt;
    int table_exists = 0;
    if (sqlite3_prepare_v2(sm->db,
                           "SELECT name FROM sqlite_master WHERE type='table' AND name='SensorData';",
                           -1, &check_stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(check_stmt) == SQLITE_ROW) {
            table_exists = 1;
        }
        sqlite3_finalize(check_stmt);
    }

    /* Tạo bảng SensorData nếu chưa tồn tại theo schema Mục 3.6 */
    const char *sql_create =
        "CREATE TABLE IF NOT EXISTS SensorData ("
        " id INTEGER NOT NULL,"
        " type INTEGER NOT NULL,"
        " value REAL NOT NULL,"
        " ts INTEGER NOT NULL"
        ");";

    char *err_msg = NULL;
    rc = sqlite3_exec(sm->db, sql_create, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        if (err_msg) {
            sqlite3_free(err_msg);
        }
        return -1;
    }

    if (!table_exists) {
        log_event("STOR", "New table SensorData created.");
    }

    /* Chuẩn bị Prepared Statement cho thao tác chèn bản ghi */
    const char *sql_insert = "INSERT INTO SensorData (id, type, value, ts) VALUES (?, ?, ?, ?);";
    rc = sqlite3_prepare_v2(sm->db, sql_insert, -1, &sm->insert, NULL);
    if (rc != SQLITE_OK) {
        return -1;
    }

    return 0;
}

int storagemgr_reconnect(storagemgr_t *sm) {
    if (!sm) return -1;

    if (sm->insert) {
        sqlite3_finalize(sm->insert);
        sm->insert = NULL;
    }
    if (sm->db) {
        sqlite3_close(sm->db);
        sm->db = NULL;
    }

    sm->state = SQL_LOST;
    log_event("STOR", "Connection to SQL server lost.");

    while (sm->attempts < 3) {
        sm->attempts++;
        sleep(1); /* Chờ 1 giây trước khi thử lại */

        int rc = sqlite3_open(sm->db_path, &sm->db);
        if (rc == SQLITE_OK) {
            sqlite3_exec(sm->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
            sqlite3_busy_timeout(sm->db, 2000);

            const char *sql_insert = "INSERT INTO SensorData (id, type, value, ts) VALUES (?, ?, ?, ?);";
            if (sqlite3_prepare_v2(sm->db, sql_insert, -1, &sm->insert, NULL) == SQLITE_OK) {
                sm->state = SQL_CONNECTED;
                sm->attempts = 0;
                log_event("STOR", "Connection to SQL server established.");
                return 0;
            }
        }
    }

    /* Quá 3 lần thử vẫn thất bại */
    sm->state = SQL_DOWN;
    log_event("STOR", "Unable to connect to SQL server.");
    return -1;
}

void storagemgr_cleanup(storagemgr_t *sm) {
    if (!sm) return;

    if (sm->insert) {
        sqlite3_finalize(sm->insert);
        sm->insert = NULL;
    }
    if (sm->db) {
        sqlite3_close(sm->db);
        sm->db = NULL;
    }
    sm->state = SQL_DOWN;
}

void *storagemgr_thread(void *arg) {
    gateway_t *gw = (gateway_t *)arg;
    storagemgr_t *sm = &gw->stor;

#ifdef DEBUG
    printf("[STORAGEMGR] Storage Manager Thread da khoi dong.\n");
#endif

    while (!gw->stop) {
        sensor_data_t data;
        int ret = sbuffer_read_storage(&gw->sbuf, &data);

        if (ret == 1) {
#ifdef DEBUG
            printf("[STORAGEMGR-DEBUG] Doc sbuffer de luu SQL: ID=%u, Value=%.2f\n",
                   data.id, data.value);
#endif
            int write_success = 0;

            while (!write_success && !gw->stop) {
                if (sm->state != SQL_CONNECTED) {
                    if (storagemgr_reconnect(sm) != 0) {
                        /* 3 lần thử thất bại -> Báo hiệu dừng toàn bộ gateway */
                        if (gw->status && shared_status_lock(gw->status) == 0) {
                            gw->status->sql_state = SQL_DOWN;
                            gw->status->sql_retries = (uint8_t)sm->attempts;
                            shared_status_unlock(gw->status);
                        }
                        gw->stop = 1;
                        sbuffer_shutdown(&gw->sbuf);
                        break;
                    }
                }

                sqlite3_bind_int(sm->insert, 1, (int)data.id);
                sqlite3_bind_int(sm->insert, 2, (int)data.type);
                sqlite3_bind_double(sm->insert, 3, data.value);
                sqlite3_bind_int64(sm->insert, 4, (sqlite3_int64)data.ts);

                int rc = sqlite3_step(sm->insert);
                sqlite3_reset(sm->insert);

                if (rc == SQLITE_DONE) {
                    write_success = 1;
                    /* Cập nhật thống kê lưu trữ vào shared memory */
                    if (gw->status && shared_status_lock(gw->status) == 0) {
                        gw->status->stored++;
                        gw->status->sql_state = SQL_CONNECTED;
                        gw->status->sql_retries = 0;
                        gw->status->sbuffer_size = (uint32_t)sbuffer_get_size(&gw->sbuf);
                        shared_status_unlock(gw->status);
                    }
                } else {
                    /* Lỗi khi ghi SQLite, kích hoạt reconnect */
                    if (storagemgr_reconnect(sm) != 0) {
                        if (gw->status && shared_status_lock(gw->status) == 0) {
                            gw->status->sql_state = SQL_DOWN;
                            gw->status->sql_retries = (uint8_t)sm->attempts;
                            shared_status_unlock(gw->status);
                        }
                        gw->stop = 1;
                        sbuffer_shutdown(&gw->sbuf);
                        break;
                    }
                }
            }
        }
        else if (ret == 0) {
            /* Shutdown và không còn bản ghi nào cần lưu */
            break;
        }
    }

#ifdef DEBUG
    printf("[STORAGEMGR] Storage Manager Thread dang dung...\n");
#endif
    return NULL;
}


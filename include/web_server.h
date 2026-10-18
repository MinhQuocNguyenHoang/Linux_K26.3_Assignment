/*
 * web_server.h
 *
 * Tiến trình Web Server độc lập (Web Process).
 * Phục vụ giao diện người dùng (HTML/CSS/JS) và các REST API đọc dữ liệu (read-only).
 * Đảm bảo cô lập lỗi hoàn toàn với Main Process.
 * Theo Mục Step 9, Step 10 và Step 11.
 */

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdint.h>
#include <unistd.h>

/*
 * Khởi động tiến trình Web Process bằng fork()
 * Lắng nghe trên 127.0.0.1:port
 * Phục vụ:
 *   GET /                 -> web/index.html
 *   GET /api/status       -> JSON từ shared memory
 *   GET /api/log?n=       -> JSON đọc n dòng cuối từ gateway.log
 *   GET /api/history?id=&n= -> JSON đọc lịch sử đo từ SQLite (read-only)
 */
pid_t web_server_start(uint16_t http_port,
                       const char *shm_name,
                       const char *fifo_path,
                       const char *log_path,
                       const char *db_path,
                       const char *web_dir);

#endif /* WEB_SERVER_H */


/*
 * log_process.h
 *
 * Quản lý ghi nhật ký hệ thống qua tiến trình con (Log Process) và đường ống FIFO (logFifo).
 * Đảm bảo ghi nhật ký tập trung, không làm chậm các luồng xử lý và ghi nguyên tử (< PIPE_BUF).
 * Định dạng theo Mục 3.8 của đề bài.
 */

#ifndef LOG_PROCESS_H
#define LOG_PROCESS_H

#include <unistd.h>
#include <stdarg.h>

#define FIFO_DEFAULT_NAME "logFifo"
#define LOG_DEFAULT_NAME  "gateway.log"

/* Khởi động tiến trình Log Process bằng fork() */
pid_t log_process_start(const char *fifo_path, const char *log_path);

/* Khởi tạo phía client gửi log (mở FIFO để ghi) */
int log_client_init(const char *fifo_path);

/* Đóng phía client gửi log */
void log_client_close(void);

/*
 * Gửi một log-event vào FIFO:
 * Định dạng truyền qua FIFO: "<nguồn> <thông điệp>\n"
 * Thực hiện bằng một lệnh write() nguyên tử duy nhất (không vượt quá PIPE_BUF = 4096).
 * Được bảo vệ bằng một pthread_mutex_t riêng để đảm bảo thread-safe giữa các luồng.
 */
void log_event(const char *source, const char *fmt, ...);

#endif /* LOG_PROCESS_H */


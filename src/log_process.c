/*
 * log_process.c
 *
 * Hiện thực tiến trình Log Process và cơ chế truyền nhận log qua named pipe (FIFO).
 * Đảm bảo:
 * 1. Log process chạy độc lập, I/O ghi đĩa không gây nghẽn các luồng xử lý.
 * 2. Ghi vào FIFO nguyên tử (kích thước < PIPE_BUF) và thread-safe qua mutex riêng.
 * 3. Log process đánh số thứ tự tuần tự tăng dần từ 1 và ghi mốc thời gian chính xác.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include "log_process.h"

/* Biến toàn cục phía client gửi log */
static int g_log_fd = -1;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Cờ dừng tiến trình log */
static volatile sig_atomic_t g_log_stop = 0;

static void log_sig_handler(int sig) {
    (void)sig;
    g_log_stop = 1;
}

/* Vòng lặp chính của tiến trình Log Process (tiến trình con sau fork) */
static void log_process_main(const char *fifo_path, const char *log_path) {
    /* Thiết lập bắt tín hiệu để kết thúc an toàn */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = log_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Mở FIFO để đọc. Sử dụng O_RDWR để fd luôn mở, tránh việc nhận EOF khi các luồng tạm đóng */
    int fifo_fd = open(fifo_path, O_RDWR);
    if (fifo_fd < 0) {
        perror("[LOG PROCESS] Khong the mo FIFO de doc");
        exit(EXIT_FAILURE);
    }

    FILE *log_file = fopen(log_path, "a");
    if (!log_file) {
        perror("[LOG PROCESS] Khong the mo file gateway.log de ghi");
        close(fifo_fd);
        exit(EXIT_FAILURE);
    }

    uint64_t seq = 1;
    char buffer[4096];
    size_t buf_len = 0;

    while (!g_log_stop) {
        ssize_t bytes_read = read(fifo_fd, buffer + buf_len, sizeof(buffer) - buf_len - 1);
        if (bytes_read > 0) {
            buf_len += bytes_read;
            buffer[buf_len] = '\0';

            /* Tách các dòng log kết thúc bằng '\n' */
            char *line_start = buffer;
            char *newline = NULL;

            while ((newline = strchr(line_start, '\n')) != NULL) {
                *newline = '\0';

                if (strlen(line_start) > 0) {
                    /* Tách nguồn và thông điệp: "<source> <thông điệp>" */
                    char source[32] = {0};
                    char message[512] = {0};

                    if (sscanf(line_start, "%31s %[^\n]", source, message) >= 1) {
                        /* Lấy thời gian hiện tại YYYY-MM-DD HH:MM:SS */
                        time_t now = time(NULL);
                        struct tm tm_info;
                        localtime_r(&now, &tm_info);
                        char time_buf[32];
                        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

                        /* Ghi vào gateway.log theo định dạng Mục 3.8:
                         * <seq> <thời điểm> <nguồn> <thông điệp>
                         */
                        fprintf(log_file, "%" PRIu64 " %s %s %s\n", seq++, time_buf, source, message);
                        fflush(log_file);
                    }
                }

                line_start = newline + 1;
            }

            /* Dồn các byte dư còn lại chưa kết thúc bằng '\n' về đầu buffer */
            size_t remaining = (buffer + buf_len) - line_start;
            if (remaining > 0) {
                memmove(buffer, line_start, remaining);
            }
            buf_len = remaining;
        } else if (bytes_read == 0) {
            /* Không còn dữ liệu tạm thời, nghỉ ngắn */
            usleep(10000);
        } else {
            if (errno == EINTR) continue;
            break;
        }
    }

    /* Đóng file và fd trước khi thoát */
    fclose(log_file);
    close(fifo_fd);
    exit(EXIT_SUCCESS);
}

pid_t log_process_start(const char *fifo_path, const char *log_path) {
    /* Tạo FIFO nếu chưa tồn tại */
    if (mkfifo(fifo_path, 0666) < 0 && errno != EEXIST) {
        perror("[MAIN] mkfifo that bai");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[MAIN] fork log process that bai");
        return -1;
    }

    if (pid == 0) {
        /* Tiến trình con: chạy vòng lặp log */
        log_process_main(fifo_path, log_path);
        exit(EXIT_SUCCESS);
    }

    /* Tiến trình cha: trả về PID của log process */
    return pid;
}

int log_client_init(const char *fifo_path) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0) {
        pthread_mutex_unlock(&g_log_mutex);
        return 0;
    }

    /* Mở FIFO phía ghi */
    g_log_fd = open(fifo_path, O_WRONLY);
    if (g_log_fd < 0) {
        perror("[LOG CLIENT] open FIFO write that bai");
        pthread_mutex_unlock(&g_log_mutex);
        return -1;
    }

    pthread_mutex_unlock(&g_log_mutex);
    return 0;
}

void log_client_close(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void log_event(const char *source, const char *fmt, ...) {
    char msg[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Chuẩn bị thông điệp gửi qua FIFO: "<nguồn> <thông điệp>\n" */
    char packet[512];
    int len = snprintf(packet, sizeof(packet), "%s %s\n", source, msg);
    if (len <= 0) return;

    /* In ra stdout nếu ở chế độ DEBUG */
#ifdef DEBUG
    printf("[LOG-%s] %s\n", source, msg);
    fflush(stdout);
#endif

    pthread_mutex_lock(&g_log_mutex);
    if (g_log_fd >= 0) {
        /* Ghi nguyên tử bằng một lệnh write duy nhất (< PIPE_BUF = 4096) */
        ssize_t ret = write(g_log_fd, packet, (size_t)len);
        (void)ret;
    }
    pthread_mutex_unlock(&g_log_mutex);
}


/*
 * main.c
 *
 * Tiến trình chính của Sensor Gateway (Main Process).
 * Nhiệm vụ:
 * 1. Khởi tạo và quản lý vòng đời toàn bộ hệ thống: sbuffer, Shared Memory, FIFO.
 * 2. Fork hai tiến trình con: Log Process và Web Process.
 * 3. Tạo ba luồng xử lý: Connection Manager, Data Manager, Storage Manager.
 * 4. Bắt và xử lý tín hiệu SIGINT, SIGTERM, SIGCHLD (tránh zombie process).
 * 5. Thực hiện quy trình tắt hệ thống có kiểm soát (Graceful Shutdown) và thu hồi tài nguyên sạch sẽ.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <errno.h>
#include "gateway.h"
#include "log_process.h"
#include "web_server.h"

/* Biến ngữ cảnh toàn cục */
gateway_t *g_gw = NULL;

/* Trình xử lý tín hiệu an toàn (Async-Signal-Safe) */
static void sig_handler(int sig) {
    if (g_gw) {
        g_gw->stop = 1;
        if (sig == SIGINT || sig == SIGTERM) {
            /* Đánh thức sbuffer để các luồng đọc không bị kẹt */
            sbuffer_shutdown(&g_gw->sbuf);
        }
    }
}

/* Xử lý tín hiệu SIGCHLD để dọn dẹp các tiến trình con đã kết thúc, chống zombie */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    errno = saved_errno;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Cú pháp: %s <cổng gốc TCP P> <cổng HTTP>\n", argv[0]);
        fprintf(stderr, "Ví dụ:   %s 1234 8080\n", argv[0]);
        return EXIT_FAILURE;
    }

    int base_port = atoi(argv[1]);
    int http_port = atoi(argv[2]);

    if (base_port <= 1024 || base_port > 65530 || http_port <= 1024 || http_port > 65535) {
        fprintf(stderr, "Lỗi: Cổng phải nằm trong khoảng hợp lệ (1025 .. 65530).\n");
        return EXIT_FAILURE;
    }

    /* Khởi tạo cấu trúc ngữ cảnh gateway */
    gateway_t gw;
    memset(&gw, 0, sizeof(gateway_t));
    gw.base_port = (uint16_t)base_port;
    gw.http_port = (uint16_t)http_port;
    gw.stop = 0;
    g_gw = &gw;

    /* Thiết lập bắt các tín hiệu kết thúc */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Thiết lập dọn dẹp tiến trình con SIGCHLD */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    printf("===============================================================\n");
    printf("         SENSOR GATEWAY - EMBEDDED LINUX SMART HOME            \n");
    printf("===============================================================\n");
    printf("[MAIN] Cổng gốc TCP: %d (Lang nghe 4 cong %d .. %d)\n",
           base_port, base_port, base_port + 3);
    printf("[MAIN] Cổng HTTP web: %d\n", http_port);

    /* 1. Tạo Shared Memory POSIX trước khi fork */
    gw.status = shared_status_create(SHM_DEFAULT_NAME);
    if (!gw.status) {
        fprintf(stderr, "[MAIN] Khong the khoi tao Shared Memory.\n");
        return EXIT_FAILURE;
    }

    /* 2. Khởi động Log Process qua fork() */
    gw.log_pid = log_process_start(FIFO_DEFAULT_NAME, LOG_DEFAULT_NAME);
    if (gw.log_pid < 0) {
        fprintf(stderr, "[MAIN] Khoi dong Log Process that bai.\n");
        shared_status_destroy(SHM_DEFAULT_NAME, gw.status);
        return EXIT_FAILURE;
    }

    /* Mở đầu ghi của FIFO cho Main Process và các thread */
    if (log_client_init(FIFO_DEFAULT_NAME) != 0) {
        fprintf(stderr, "[MAIN] Khong the ket noi den FIFO logFifo.\n");
        kill(gw.log_pid, SIGTERM);
        waitpid(gw.log_pid, NULL, 0);
        shared_status_destroy(SHM_DEFAULT_NAME, gw.status);
        return EXIT_FAILURE;
    }

    /* Ghi log khởi động hệ thống theo Mục 3.8 */
    log_event("MAIN", "Gateway started (TCP base port %u, HTTP port %u)",
              (unsigned int)gw.base_port, (unsigned int)gw.http_port);

    /* 3. Khởi tạo sbuffer dùng chung giữa 3 thread */
    if (sbuffer_init(&gw.sbuf) != 0) {
        fprintf(stderr, "[MAIN] Khoi tao sbuffer that bai.\n");
        gw.stop = 1;
    }

    /* 4. Nạp file cấu hình room_sensor.map cho Data Manager */
    if (datamgr_load_map(&gw.data, "room_sensor.map") != 0) {
        fprintf(stderr, "[MAIN] Canh bao: Khong nap duoc room_sensor.map mac dinh.\n");
    }

    /* 5. Khởi tạo cơ sở dữ liệu SQLite cho Storage Manager */
    if (storagemgr_init(&gw.stor, "gateway.db") != 0) {
        fprintf(stderr, "[MAIN] Canh bao: Khoi tao SQLite gap su co (se thu ket noi lai trong thread).\n");
    }

    /* 6. Khởi tạo socket lắng nghe trên 4 cổng cho Connection Manager */
    if (connmgr_init(&gw.conn, gw.base_port) != 0) {
        fprintf(stderr, "[MAIN] Khoi tao socket Connection Manager that bai.\n");
        gw.stop = 1;
    }

    /* 7. Khởi động Web Process qua fork() */
    gw.web_pid = web_server_start(gw.http_port, SHM_DEFAULT_NAME,
                                  FIFO_DEFAULT_NAME, LOG_DEFAULT_NAME,
                                  "gateway.db", "web");
    if (gw.web_pid < 0) {
        fprintf(stderr, "[MAIN] Khoi dong Web Server process that bai.\n");
    }

    /* 8. Khởi chạy 3 thread: connmgr, datamgr, storagemgr */
    pthread_t tid_conn, tid_data, tid_stor;
    int rc_conn = pthread_create(&tid_conn, NULL, connmgr_thread, &gw);
    int rc_data = pthread_create(&tid_data, NULL, datamgr_thread, &gw);
    int rc_stor = pthread_create(&tid_stor, NULL, storagemgr_thread, &gw);

    if (rc_conn != 0 || rc_data != 0 || rc_stor != 0) {
        fprintf(stderr, "[MAIN] pthread_create gap loi, dang thoat...\n");
        gw.stop = 1;
    }

    printf("[MAIN] He thong da khoi dong thanh cong tat ca cac tien trinh va luong.\n");
    printf("[MAIN] Mo trinh duyet truy cap: http://127.0.0.1:%d\n", http_port);
    printf("[MAIN] Nhan Ctrl+C de ket thuc he thong co kiem soat.\n");

    /* Vòng lặp chờ cờ stop */
    while (!gw.stop) {
        sleep(1);
    }

    /* =====================================================================
     * QUY TRÌNH KẾT THÚC CÓ KIỂM SOÁT (Step 7 & Step 11)
     * ===================================================================== */
    printf("\n[MAIN] Bat dau quy trinh shutdown gateway...\n");
    log_event("MAIN", "Gateway shutting down (signal received)");

    /* 1. Phát tín hiệu dừng sbuffer để giải phóng các luồng đang đợi */
    sbuffer_shutdown(&gw.sbuf);

    /* 2. Đợi 3 luồng hoàn thành và thu hồi tài nguyên (pthread_join) */
    if (rc_conn == 0) pthread_join(tid_conn, NULL);
    if (rc_data == 0) pthread_join(tid_data, NULL);
    if (rc_stor == 0) pthread_join(tid_stor, NULL);
    printf("[MAIN] Ba luong da ket thuc an toan.\n");

    /* 3. Đóng các socket lắng nghe và kết nối */
    connmgr_cleanup(&gw.conn);

    /* 4. Đóng cơ sở dữ liệu SQLite */
    storagemgr_cleanup(&gw.stor);

    /* 5. Giải phóng tài nguyên sbuffer */
    sbuffer_cleanup(&gw.sbuf);

    /* 6. Dừng Web Process và chờ thu hồi để tránh zombie */
    if (gw.web_pid > 0) {
        kill(gw.web_pid, SIGTERM);
        waitpid(gw.web_pid, NULL, 0);
        printf("[MAIN] Web process da thu hoi.\n");
    }

    /* 7. Đóng kết nối gửi log và dừng Log Process */
    log_client_close();
    if (gw.log_pid > 0) {
        kill(gw.log_pid, SIGTERM);
        waitpid(gw.log_pid, NULL, 0);
        printf("[MAIN] Log process da thu hoi.\n");
    }

    /* 8. Hủy vùng nhớ chia sẻ POSIX và xóa FIFO */
    shared_status_destroy(SHM_DEFAULT_NAME, gw.status);
    unlink(FIFO_DEFAULT_NAME);

    printf("[MAIN] Hoan tat thu dọn toan bo tai nguyen. Gateway thoat thanh cong.\n");
    return EXIT_SUCCESS;
}


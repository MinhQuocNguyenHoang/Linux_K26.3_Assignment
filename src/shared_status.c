/*
 * shared_status.c
 *
 * Hiện thực quản lý vùng nhớ chia sẻ POSIX Shared Memory và Mutex Robust liên tiến trình.
 * Đảm bảo:
 * 1. Web process đọc dữ liệu trạng thái thời gian thực mà không làm ảnh hưởng các luồng của main process.
 * 2. Bảo vệ tranh chấp dữ liệu bằng pthread_mutex_t có cờ PTHREAD_PROCESS_SHARED.
 * 3. Thuộc tính PTHREAD_MUTEX_ROBUST giúp tự khôi phục (EOWNERDEAD -> pthread_mutex_consistent)
 *    nếu một tiến trình bất ngờ bị crash (kill -9) khi đang giữ mutex.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include "shared_status.h"

gateway_status_t *shared_status_create(const char *name) {
    if (!name) name = SHM_DEFAULT_NAME;

    /* Xóa vùng nhớ cũ nếu còn tồn đọng từ lần chạy trước */
    shm_unlink(name);

    /* Mở/tạo mới vùng nhớ chia sẻ POSIX với quyền đọc/ghi */
    int shm_fd = shm_open(name, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd < 0) {
        perror("[SHM] shm_open O_CREAT that bai");
        return NULL;
    }

    /* Đặt kích thước vùng nhớ chia sẻ */
    if (ftruncate(shm_fd, sizeof(gateway_status_t)) < 0) {
        perror("[SHM] ftruncate that bai");
        close(shm_fd);
        shm_unlink(name);
        return NULL;
    }

    /* Ánh xạ vào không gian địa chỉ ảo của tiến trình */
    void *ptr = mmap(NULL, sizeof(gateway_status_t),
                     PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (ptr == MAP_FAILED) {
        perror("[SHM] mmap that bai");
        shm_unlink(name);
        return NULL;
    }

    gateway_status_t *status = (gateway_status_t *)ptr;
    memset(status, 0, sizeof(gateway_status_t));

    /* Khởi tạo Mutex liên tiến trình (PTHREAD_PROCESS_SHARED) và ROBUST */
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);

    if (pthread_mutex_init(&status->lock, &mattr) != 0) {
        perror("[SHM] pthread_mutex_init that bai");
        pthread_mutexattr_destroy(&mattr);
        munmap(status, sizeof(gateway_status_t));
        shm_unlink(name);
        return NULL;
    }
    pthread_mutexattr_destroy(&mattr);

    status->start_time = time(NULL);
    status->sql_state = SQL_LOST; /* Ban đầu chưa kết nối */
    status->sql_retries = 0;
    status->sensor_count = 0;

    return status;
}

gateway_status_t *shared_status_open(const char *name) {
    if (!name) name = SHM_DEFAULT_NAME;

    int shm_fd = shm_open(name, O_RDWR, 0666);
    if (shm_fd < 0) {
        return NULL;
    }

    void *ptr = mmap(NULL, sizeof(gateway_status_t),
                     PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);

    if (ptr == MAP_FAILED) {
        return NULL;
    }

    return (gateway_status_t *)ptr;
}

void shared_status_detach(gateway_status_t *status) {
    if (status) {
        munmap(status, sizeof(gateway_status_t));
    }
}

void shared_status_destroy(const char *name, gateway_status_t *status) {
    if (status) {
        pthread_mutex_destroy(&status->lock);
        munmap(status, sizeof(gateway_status_t));
    }
    if (name) {
        shm_unlink(name);
    }
}

int shared_status_lock(gateway_status_t *status) {
    if (!status) return -1;

    int rc = pthread_mutex_lock(&status->lock);
    if (rc == EOWNERDEAD) {
        /*
         * Tiến trình trước đó giữ mutex bị kết thúc bất thường (ví dụ: web process bị kill -9).
         * Gọi pthread_mutex_consistent() để khôi phục trạng thái nhất quán theo chuẩn POSIX.
         */
        pthread_mutex_consistent(&status->lock);
        return 0;
    }

    return (rc == 0) ? 0 : -1;
}

int shared_status_unlock(gateway_status_t *status) {
    if (!status) return -1;
    return (pthread_mutex_unlock(&status->lock) == 0) ? 0 : -1;
}


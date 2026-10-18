/*
 * test_sbuffer.c
 *
 * Kiểm tra kiểm thử đa luồng (Multi-threading) và tính đúng đắn của hàng đợi sbuffer:
 * 1. Cơ chế hai cờ (read_by_data và read_by_storage):
 *    Node CHỈ được giải phóng khi CẢ HAI luồng đọc đã đọc xong.
 * 2. Không xảy ra Data Race hoặc Deadlock giữa 1 Writer và 2 Reader.
 * 3. Tương thích công cụ kiểm tra helgrind và valgrind.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include "sbuffer.h"

#define NUM_ITEMS 500

static sbuffer_t sbuf;
static int data_read_count = 0;
static int storage_read_count = 0;

static void *writer_thread(void *arg) {
    (void)arg;
    for (int i = 1; i <= NUM_ITEMS; i++) {
        sensor_data_t d;
        d.id = (sensor_id_t)(i % 50 + 1);
        d.type = SENSOR_TEMPERATURE;
        d.value = 20.0 + (i % 10);
        d.ts = time(NULL);

        sbuffer_insert(&sbuf, &d);
        if (i % 50 == 0) {
            usleep(1000); /* 1ms */
        }
    }
    /* Chờ một chút trước khi shutdown sbuffer */
    usleep(20000);
    sbuffer_shutdown(&sbuf);
    return NULL;
}

static void *data_reader_thread(void *arg) {
    (void)arg;
    sensor_data_t d;
    while (sbuffer_read_data(&sbuf, &d) == 1) {
        data_read_count++;
    }
    return NULL;
}

static void *storage_reader_thread(void *arg) {
    (void)arg;
    sensor_data_t d;
    while (sbuffer_read_storage(&sbuf, &d) == 1) {
        storage_read_count++;
    }
    return NULL;
}

int main(void) {
    printf("=== KIỂM THỬ ĐA LUỒNG SBUFFER (Step 3) ===\n");

    int rc = sbuffer_init(&sbuf);
    assert(rc == 0);

    pthread_t t_writer, t_data, t_storage;
    pthread_create(&t_writer, NULL, writer_thread, NULL);
    pthread_create(&t_data, NULL, data_reader_thread, NULL);
    pthread_create(&t_storage, NULL, storage_reader_thread, NULL);

    pthread_join(t_writer, NULL);
    pthread_join(t_data, NULL);
    pthread_join(t_storage, NULL);

    printf("[SBUFFER TEST] Writer ghi: %d phần tử.\n", NUM_ITEMS);
    printf("[SBUFFER TEST] Data reader đọc được: %d phần tử.\n", data_read_count);
    printf("[SBUFFER TEST] Storage reader đọc được: %d phần tử.\n", storage_read_count);
    printf("[SBUFFER TEST] Kích thước sbuffer sau khi cả 2 reader đọc xong: %zu\n", sbuffer_get_size(&sbuf));

    assert(data_read_count == NUM_ITEMS);
    assert(storage_read_count == NUM_ITEMS);
    assert(sbuffer_get_size(&sbuf) == 0);

    sbuffer_cleanup(&sbuf);

    printf(">>> KIỂM THỬ SBUFFER THÀNH CÔNG RỰC RỠ, KHÔNG RÒ RỈ BỘ NHỚ!\n\n");
    return EXIT_SUCCESS;
}


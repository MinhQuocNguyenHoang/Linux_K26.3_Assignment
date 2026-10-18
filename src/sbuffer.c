/*
 * sbuffer.c
 *
 * Hiện thực hàng đợi dùng chung sbuffer (Shared Buffer) an toàn luồng (thread-safe).
 * Đồng bộ bằng pthread_mutex_t và pthread_cond_t.
 * Mỗi phần tử có hai cờ read_by_data và read_by_storage.
 * Một phần tử CHỈ bị hủy khỏi bộ nhớ khi CẢ HAI luồng Data manager và Storage manager đã đọc.
 */

#include <stdlib.h>
#include <string.h>
#include "sbuffer.h"

int sbuffer_init(sbuffer_t *sbuf) {
    if (!sbuf) return -1;
    sbuf->head = NULL;
    sbuf->tail = NULL;
    sbuf->size = 0;
    sbuf->shutdown = false;

    if (pthread_mutex_init(&sbuf->lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&sbuf->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&sbuf->lock);
        return -1;
    }
    return 0;
}

int sbuffer_insert(sbuffer_t *sbuf, const sensor_data_t *data) {
    if (!sbuf || !data) return -1;

    sbuffer_node_t *node = (sbuffer_node_t *)malloc(sizeof(sbuffer_node_t));
    if (!node) return -1;

    node->data = *data;
    node->read_by_data = false;
    node->read_by_storage = false;
    node->next = NULL;

    pthread_mutex_lock(&sbuf->lock);

    if (sbuf->shutdown) {
        pthread_mutex_unlock(&sbuf->lock);
        free(node);
        return -1;
    }

    if (sbuf->tail == NULL) {
        sbuf->head = node;
        sbuf->tail = node;
    } else {
        sbuf->tail->next = node;
        sbuf->tail = node;
    }
    sbuf->size++;

    /* Đánh thức tất cả các thread reader đang bị chặn chờ dữ liệu */
    pthread_cond_broadcast(&sbuf->not_empty);

    pthread_mutex_unlock(&sbuf->lock);
    return 0;
}

/* Hàm nội bộ: dọn dẹp các node đã được cả 2 bên đọc xong (phải được gọi trong khi giữ lock) */
static void sbuffer_purge_read_nodes(sbuffer_t *sbuf) {
    sbuffer_node_t *prev = NULL;
    sbuffer_node_t *curr = sbuf->head;

    while (curr != NULL) {
        if (curr->read_by_data && curr->read_by_storage) {
            sbuffer_node_t *to_free = curr;
            if (prev == NULL) {
                sbuf->head = curr->next;
            } else {
                prev->next = curr->next;
            }
            if (to_free == sbuf->tail) {
                sbuf->tail = prev;
            }
            curr = curr->next;
            free(to_free);
            if (sbuf->size > 0) {
                sbuf->size--;
            }
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
}

int sbuffer_read_data(sbuffer_t *sbuf, sensor_data_t *out) {
    if (!sbuf || !out) return -1;

    pthread_mutex_lock(&sbuf->lock);

    while (1) {
        /* Tìm phần tử đầu tiên mà Data Manager chưa đọc */
        sbuffer_node_t *curr = sbuf->head;
        while (curr != NULL && curr->read_by_data) {
            curr = curr->next;
        }

        if (curr != NULL) {
            *out = curr->data;
            curr->read_by_data = true;

            /* Thu hồi các node đã hoàn thành đọc bởi cả 2 reader */
            sbuffer_purge_read_nodes(sbuf);

            pthread_mutex_unlock(&sbuf->lock);
            return 1;
        }

        /* Không có phần tử chưa đọc nào cho Data manager */
        if (sbuf->shutdown) {
            pthread_mutex_unlock(&sbuf->lock);
            return 0; /* Hàng đợi đã dừng và không còn dữ liệu mới */
        }

        /* Chờ tín hiệu từ Connection manager khi có dữ liệu mới hoặc shutdown */
        pthread_cond_wait(&sbuf->not_empty, &sbuf->lock);
    }
}

int sbuffer_read_storage(sbuffer_t *sbuf, sensor_data_t *out) {
    if (!sbuf || !out) return -1;

    pthread_mutex_lock(&sbuf->lock);

    while (1) {
        /* Tìm phần tử đầu tiên mà Storage Manager chưa đọc */
        sbuffer_node_t *curr = sbuf->head;
        while (curr != NULL && curr->read_by_storage) {
            curr = curr->next;
        }

        if (curr != NULL) {
            *out = curr->data;
            curr->read_by_storage = true;

            /* Thu hồi các node đã hoàn thành đọc bởi cả 2 reader */
            sbuffer_purge_read_nodes(sbuf);

            pthread_mutex_unlock(&sbuf->lock);
            return 1;
        }

        /* Không có phần tử chưa đọc nào cho Storage manager */
        if (sbuf->shutdown) {
            pthread_mutex_unlock(&sbuf->lock);
            return 0; /* Hàng đợi đã dừng và không còn dữ liệu mới */
        }

        /* Chờ tín hiệu từ Connection manager khi có dữ liệu mới hoặc shutdown */
        pthread_cond_wait(&sbuf->not_empty, &sbuf->lock);
    }
}

void sbuffer_shutdown(sbuffer_t *sbuf) {
    if (!sbuf) return;
    pthread_mutex_lock(&sbuf->lock);
    sbuf->shutdown = true;
    pthread_cond_broadcast(&sbuf->not_empty);
    pthread_mutex_unlock(&sbuf->lock);
}

void sbuffer_cleanup(sbuffer_t *sbuf) {
    if (!sbuf) return;

    pthread_mutex_lock(&sbuf->lock);
    sbuffer_node_t *curr = sbuf->head;
    while (curr != NULL) {
        sbuffer_node_t *tmp = curr;
        curr = curr->next;
        free(tmp);
    }
    sbuf->head = NULL;
    sbuf->tail = NULL;
    sbuf->size = 0;
    pthread_mutex_unlock(&sbuf->lock);

    pthread_mutex_destroy(&sbuf->lock);
    pthread_cond_destroy(&sbuf->not_empty);
}

size_t sbuffer_get_size(sbuffer_t *sbuf) {
    if (!sbuf) return 0;
    pthread_mutex_lock(&sbuf->lock);
    size_t sz = sbuf->size;
    pthread_mutex_unlock(&sbuf->lock);
    return sz;
}


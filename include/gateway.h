/*
 * gateway.h
 *
 * Cấu trúc ngữ cảnh gốc toàn cục của Sensor Gateway (Main Process).
 * Chứa các thành phần quản lý luồng, IPC, socket và tín hiệu kết thúc.
 * Cung cấp sẵn theo Mục 3.11 của đề bài.
 */

#ifndef GATEWAY_H
#define GATEWAY_H

#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include "sbuffer.h"
#include "connmgr.h"
#include "datamgr.h"
#include "storagemgr.h"
#include "shared_status.h"

/* Ngữ cảnh gốc của main process */
typedef struct gateway_s {
    uint16_t base_port, http_port;
    sbuffer_t sbuf;
    connmgr_t conn;
    datamgr_t data;
    storagemgr_t stor;
    gateway_status_t *status; /* Vùng nhớ chia sẻ POSIX */
    pid_t log_pid, web_pid;
    volatile sig_atomic_t stop;
} gateway_t;

/* Con trỏ toàn cục tới ngữ cảnh gateway (cho signal handler) */
extern gateway_t *g_gw;

#endif /* GATEWAY_H */


/*
 * connmgr.c
 *
 * Hiện thực Connection Manager Thread.
 * Sử dụng cơ chế I/O multiplexing với poll() để phục vụ đồng thời 4 socket lắng nghe
 * (cho 4 giao thức TBP, HLP, EVP, SGP) và tối đa MAX_CONN kết nối sensor node.
 * Đảm bảo:
 * 1. Không dùng polling bận (CPU 0% khi nhàn rỗi).
 * 2. Phục vụ song song nhiều kết nối, không phụ thuộc ranh giới gói tin của TCP byte-stream.
 * 3. Tự động tái đồng bộ và ghi log khi gặp frame hỏng.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "connmgr.h"
#include "gateway.h"
#include "log_process.h"
#include "shared_status.h"

/* Thiết lập non-blocking cho socket descriptor */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int connmgr_init(connmgr_t *cm, uint16_t base_port) {
    if (!cm) return -1;

    memset(cm, 0, sizeof(connmgr_t));
    for (int i = 0; i < 4; i++) {
        cm->listen_fd[i] = -1;
    }
    for (int i = 0; i < MAX_CONN; i++) {
        cm->conns[i].fd = -1;
    }

    for (int i = 0; i < 4; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("[CONNMGR] socket() that bai");
            connmgr_cleanup(cm);
            return -1;
        }

        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(fd);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(base_port + ADAPTERS[i]->port_offset);

        if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("[CONNMGR] bind() that bai");
            close(fd);
            connmgr_cleanup(cm);
            return -1;
        }

        if (listen(fd, 32) < 0) {
            perror("[CONNMGR] listen() that bai");
            close(fd);
            connmgr_cleanup(cm);
            return -1;
        }

        cm->listen_fd[i] = fd;
#ifdef DEBUG
        printf("[CONNMGR] Dang lang nghe giao thuc %s tai cong %u\n",
               ADAPTERS[i]->name, base_port + ADAPTERS[i]->port_offset);
#endif
    }

    return 0;
}

void connmgr_cleanup(connmgr_t *cm) {
    if (!cm) return;

    for (int i = 0; i < 4; i++) {
        if (cm->listen_fd[i] >= 0) {
            close(cm->listen_fd[i]);
            cm->listen_fd[i] = -1;
        }
    }

    for (int i = 0; i < MAX_CONN; i++) {
        if (cm->conns[i].fd >= 0) {
            close(cm->conns[i].fd);
            cm->conns[i].fd = -1;
        }
    }
    cm->n_conns = 0;
}

/* Đóng và thu hồi slot kết nối */
static void close_connection(gateway_t *gw, connection_t *c) {
    if (c->fd < 0) return;

    if (c->identified) {
        log_event("CONN", "The sensor node with %u has closed the connection", c->sensor_id);

        /* Cập nhật trạng thái ngắt kết nối vào shared memory */
        if (gw->status && shared_status_lock(gw->status) == 0) {
            for (uint16_t i = 0; i < gw->status->sensor_count; i++) {
                if (gw->status->sensors[i].id == c->sensor_id) {
                    gw->status->sensors[i].connected = false;
                    break;
                }
            }
            shared_status_unlock(gw->status);
        }
    }

    close(c->fd);
    c->fd = -1;
    c->adapter = NULL;
    c->rx_len = 0;
    c->identified = false;
    c->sensor_id = 0;
    if (gw->conn.n_conns > 0) {
        gw->conn.n_conns--;
    }
}

void *connmgr_thread(void *arg) {
    gateway_t *gw = (gateway_t *)arg;
    connmgr_t *cm = &gw->conn;

#ifdef DEBUG
    printf("[CONNMGR] Connection Manager Thread da khoi dong.\n");
#endif

    /* Cấu trúc mảng pollfd: 4 socket lắng nghe + MAX_CONN socket client */
    struct pollfd pfds[4 + MAX_CONN];

    while (!gw->stop) {
        int pfd_count = 0;

        /* Đăng ký 4 socket lắng nghe */
        for (int i = 0; i < 4; i++) {
            if (cm->listen_fd[i] >= 0) {
                pfds[pfd_count].fd = cm->listen_fd[i];
                pfds[pfd_count].events = POLLIN;
                pfds[pfd_count].revents = 0;
                pfd_count++;
            }
        }

        /* Đăng ký các socket client đang kết nối */
        int client_pfd_map[MAX_CONN];
        for (int i = 0; i < MAX_CONN; i++) {
            if (cm->conns[i].fd >= 0) {
                client_pfd_map[pfd_count] = i;
                pfds[pfd_count].fd = cm->conns[i].fd;
                pfds[pfd_count].events = POLLIN;
                pfds[pfd_count].revents = 0;
                pfd_count++;
            }
        }

        /* Chờ sự kiện I/O với timeout 500ms để kiểm tra cờ gw->stop */
        int ready = poll(pfds, (nfds_t)pfd_count, 500);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("[CONNMGR] poll() that bai");
            break;
        }
        if (ready == 0) continue; /* Hết timeout, vòng lại kiểm tra cờ stop */

        /* 1. Xử lý kết nối mới trên các cổng lắng nghe */
        for (int i = 0; i < 4; i++) {
            if (pfds[i].revents & POLLIN) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(cm->listen_fd[i], (struct sockaddr *)&client_addr, &client_len);

                if (client_fd < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        log_event("CONN", "Connection attempt failed on %s port (accept error)",
                                  ADAPTERS[i]->name);
                    }
                    continue;
                }

                set_nonblocking(client_fd);

                /* Kiểm tra số lượng kết nối tối đa */
                if (cm->n_conns >= MAX_CONN) {
                    log_event("CONN", "Connection attempt failed on %s port (too many connections)",
                              ADAPTERS[i]->name);
                    close(client_fd);
                    continue;
                }

                /* Tìm slot trống cho kết nối mới */
                int slot = -1;
                for (int s = 0; s < MAX_CONN; s++) {
                    if (cm->conns[s].fd < 0) {
                        slot = s;
                        break;
                    }
                }

                if (slot >= 0) {
                    cm->conns[slot].fd = client_fd;
                    cm->conns[slot].adapter = ADAPTERS[i];
                    cm->conns[slot].rx_len = 0;
                    cm->conns[slot].identified = false;
                    cm->conns[slot].sensor_id = 0;
                    cm->n_conns++;
#ifdef DEBUG
                    printf("[CONNMGR] Chap nhan ket noi moi tren cong %s (fd=%d, slot=%d)\n",
                           ADAPTERS[i]->name, client_fd, slot);
#endif
                } else {
                    close(client_fd);
                }
            }
        }

        /* 2. Xử lý nhận dữ liệu từ các client đã kết nối */
        for (int p = 4; p < pfd_count; p++) {
            if (pfds[p].revents & (POLLIN | POLLHUP | POLLERR)) {
                int slot = client_pfd_map[p];
                connection_t *c = &cm->conns[slot];

                if (pfds[p].revents & (POLLHUP | POLLERR)) {
                    close_connection(gw, c);
                    continue;
                }

                if (pfds[p].revents & POLLIN) {
                    size_t space_left = sizeof(c->rx) - c->rx_len;
                    if (space_left == 0) {
                        /* Buffer đầy mà không parse được -> Frame sai kích thước */
                        log_event("CONN", "Received malformed frame on %s port (bad length)",
                                  c->adapter->name);
                        close_connection(gw, c);
                        continue;
                    }

                    ssize_t bytes_recv = recv(c->fd, c->rx + c->rx_len, space_left, 0);
                    if (bytes_recv > 0) {
                        c->rx_len += (size_t)bytes_recv;

                        /* Lặp giải mã cho đến khi hết frame hoặc cần thêm byte */
                        while (c->rx_len > 0) {
                            sensor_data_t data;
                            size_t consumed = 0;
                            parse_result_t res = c->adapter->parse(c->rx, c->rx_len, &data, &consumed);

                            if (res == PARSE_OK) {
                                /* Nếu là frame hợp lệ đầu tiên của node -> Ghi log mở kết nối */
                                if (!c->identified) {
                                    c->identified = true;
                                    c->sensor_id = data.id;
                                    log_event("CONN", "A sensor node with %u has opened a new connection",
                                              (unsigned int)data.id);
                                }

                                /* Gửi phản hồi ACK nếu giao thức yêu cầu (ví dụ: SGP) */
                                if (c->adapter->build_reply) {
                                    uint8_t reply[16];
                                    size_t rlen = c->adapter->build_reply(PARSE_OK, reply, sizeof(reply));
                                    if (rlen > 0) {
                                        send(c->fd, reply, rlen, 0);
                                    }
                                }

                                /* Đưa dữ liệu đã chuẩn hóa vào hàng đợi sbuffer */
                                sbuffer_insert(&gw->sbuf, &data);

                                /* Cập nhật thống kê nhận gói tin vào shared memory */
                                if (gw->status && shared_status_lock(gw->status) == 0) {
                                    gw->status->received++;
                                    gw->status->sbuffer_size = (uint32_t)sbuffer_get_size(&gw->sbuf);
                                    shared_status_unlock(gw->status);
                                }

                                /* Dời phần byte còn lại về đầu buffer */
                                if (consumed > 0 && consumed <= c->rx_len) {
                                    memmove(c->rx, c->rx + consumed, c->rx_len - consumed);
                                    c->rx_len -= consumed;
                                } else {
                                    c->rx_len = 0;
                                }
                            }
                            else if (res == PARSE_NEED_MORE) {
                                /* Cần thêm byte, dừng vòng lặp chờ lần recv tiếp theo */
                                break;
                            }
                            else { /* PARSE_ERROR */
                                const char *reason = "bad length";
                                if (strcmp(c->adapter->name, "SGP") == 0) {
                                    reason = "bad CRC";
                                }

                                if (!c->identified) {
                                    log_event("CONN", "Connection attempt failed on %s port (malformed first frame)",
                                              c->adapter->name);
                                    close_connection(gw, c);
                                    break;
                                } else {
                                    log_event("CONN", "Received malformed frame on %s port (%s)",
                                              c->adapter->name, reason);
                                }

                                /* Gửi phản hồi NAK nếu giao thức hỗ trợ */
                                if (c->adapter->build_reply) {
                                    uint8_t reply[16];
                                    size_t rlen = c->adapter->build_reply(PARSE_ERROR, reply, sizeof(reply));
                                    if (rlen > 0) {
                                        send(c->fd, reply, rlen, 0);
                                    }
                                }

                                /* Bỏ qua các byte bị hỏng để tìm frame kế tiếp */
                                if (consumed > 0 && consumed <= c->rx_len) {
                                    memmove(c->rx, c->rx + consumed, c->rx_len - consumed);
                                    c->rx_len -= consumed;
                                } else {
                                    c->rx_len = 0;
                                    break;
                                }
                            }
                        }
                    } else if (bytes_recv == 0) {
                        /* Client chủ động đóng kết nối (EOF) */
                        close_connection(gw, c);
                    } else {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            close_connection(gw, c);
                        }
                    }
                }
            }
        }
    }

#ifdef DEBUG
    printf("[CONNMGR] Connection Manager Thread dang dung...\n");
#endif
    return NULL;
}


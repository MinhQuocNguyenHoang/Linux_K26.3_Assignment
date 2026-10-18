/*
 * web_server.c
 *
 * Hiện thực tiến trình Web Server độc lập (Web Process).
 * Đặc điểm kiến trúc:
 * 1. Chạy trên process con riêng biệt để hoàn toàn cô lập lỗi bộ nhớ hoặc I/O mạng với Main Process.
 * 2. Phục vụ HTTP/1.1: GET /, /api/status, /api/log?n=, /api/history?id=&n=
 * 3. Đọc trạng thái từ Shared Memory qua cơ chế snapshot nhanh (giữ robust mutex trong tích tắc).
 * 4. Đọc nhật ký trực tiếp từ file gateway.log (tuyệt đối không đọc FIFO để tránh cướp gói tin của log process).
 * 5. Đọc lịch sử từ SQLite ở chế độ READ-ONLY (SQLITE_OPEN_READONLY).
 * 6. Bỏ qua SIGPIPE để tránh crash khi trình duyệt đóng tab đột ngột.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sqlite3.h>
#include "web_server.h"
#include "shared_status.h"
#include "log_process.h"

static volatile sig_atomic_t g_web_stop = 0;

static void web_sig_handler(int sig) {
    (void)sig;
    g_web_stop = 1;
}

/* Helper lấy tên chuỗi của sensor type */
static const char *get_type_name(sensor_type_t type) {
    switch (type) {
        case SENSOR_TEMPERATURE: return "temperature";
        case SENSOR_HUMIDITY:    return "humidity";
        case SENSOR_MOTION:      return "motion";
        case SENSOR_DOOR:        return "door";
        case SENSOR_SMOKE_GAS:   return "smoke_gas";
        default:                 return "unknown";
    }
}

static const char *get_type_unit(sensor_type_t type) {
    switch (type) {
        case SENSOR_TEMPERATURE: return "°C";
        case SENSOR_HUMIDITY:    return "%RH";
        case SENSOR_SMOKE_GAS:   return "ppm";
        default:                 return "";
    }
}

static const char *get_state_str(sensor_state_t state, sensor_type_t type, double val, double avg) {
    if (state == STATE_COLLECTING) return "đang thu thập";
    if (state == STATE_NORMAL) return "bình thường";
    if (type == SENSOR_TEMPERATURE) {
        return (avg > 20.0) ? "quá nóng" : "quá lạnh";
    }
    if (type == SENSOR_HUMIDITY) {
        return (avg > 70.0) ? "quá ẩm" : "quá khô";
    }
    if (type == SENSOR_MOTION) return "phát hiện chuyển động";
    if (type == SENSOR_DOOR) return (val >= 0.5) ? "cửa mở" : "cửa đóng";
    if (type == SENSOR_SMOKE_GAS) return "báo động khói/gas";
    return "cảnh báo";
}

/* Gửi phản hồi HTTP hoàn chỉnh */
static void send_http_response(int fd, int status_code, const char *status_text,
                               const char *content_type, const char *body, size_t body_len) {
    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "Access-Control-Allow-Origin: *\r\n"
                        "\r\n",
                        status_code, status_text, content_type, body_len);
    if (hlen > 0) {
        send(fd, header, (size_t)hlen, MSG_NOSIGNAL);
    }
    if (body && body_len > 0) {
        send(fd, body, body_len, MSG_NOSIGNAL);
    }
}

/* Xử lý GET / : Trả về web/index.html */
static void handle_get_index(int client_fd, const char *web_dir) {
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/index.html", web_dir);

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        const char *not_found = "<h1>404 Not Found - Missing index.html</h1>";
        send_http_response(client_fd, 404, "Not Found", "text/html", not_found, strlen(not_found));
        log_event("WEB", "Web server received an invalid request (status code 404).");
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = (char *)malloc(fsize + 1);
    if (buf) {
        size_t n = fread(buf, 1, fsize, f);
        buf[n] = '\0';
        send_http_response(client_fd, 200, "OK", "text/html; charset=utf-8", buf, n);
        free(buf);
    } else {
        const char *err = "500 Internal Server Error";
        send_http_response(client_fd, 500, "Internal Error", "text/plain", err, strlen(err));
    }
    fclose(f);
}

/* Xử lý GET /api/status */
static void handle_api_status(int client_fd, gateway_status_t *shm_status) {
    if (!shm_status) {
        const char *err = "{\"error\":\"Shared memory unavailable\"}";
        send_http_response(client_fd, 500, "Internal Error", "application/json", err, strlen(err));
        return;
    }

    /* Sao chép nhanh dữ liệu từ shared memory ra biến cục bộ */
    gateway_status_t snapshot;
    if (shared_status_lock(shm_status) == 0) {
        memcpy(&snapshot, shm_status, sizeof(gateway_status_t));
        shared_status_unlock(shm_status);
    } else {
        const char *err = "{\"error\":\"Lock failed\"}";
        send_http_response(client_fd, 500, "Internal Error", "application/json", err, strlen(err));
        return;
    }

    /* Tạo JSON trạng thái */
    char *json = (char *)malloc(16384);
    if (!json) return;

    time_t now = time(NULL);
    long uptime = (long)(now - snapshot.start_time);
    const char *sql_str = (snapshot.sql_state == SQL_CONNECTED) ? "SQL_CONNECTED" :
                          (snapshot.sql_state == SQL_LOST) ? "SQL_LOST" : "SQL_DOWN";

    int offset = snprintf(json, 16384,
        "{\n"
        "  \"gateway\": {\n"
        "    \"uptime\": %ld,\n"
        "    \"received\": %" PRIu64 ",\n"
        "    \"processed\": %" PRIu64 ",\n"
        "    \"stored\": %" PRIu64 ",\n"
        "    \"sbuffer_size\": %u,\n"
        "    \"sql_state\": \"%s\",\n"
        "    \"sql_retries\": %u,\n"
        "    \"sensor_count\": %u\n"
        "  },\n"
        "  \"sensors\": [\n",
        uptime, snapshot.received, snapshot.processed, snapshot.stored,
        snapshot.sbuffer_size, sql_str, snapshot.sql_retries, snapshot.sensor_count);

    for (uint16_t i = 0; i < snapshot.sensor_count && offset < 15000; i++) {
        sensor_status_t *s = &snapshot.sensors[i];
        const char *state_text = get_state_str(s->state, s->type, s->last_value, s->running_avg);
        const char *type_name = get_type_name(s->type);
        const char *unit = get_type_unit(s->type);

        offset += snprintf(json + offset, 16384 - offset,
            "    {\n"
            "      \"id\": %u,\n"
            "      \"room\": %u,\n"
            "      \"type\": \"%s\",\n"
            "      \"type_id\": %d,\n"
            "      \"last_value\": %.2f,\n"
            "      \"unit\": \"%s\",\n"
            "      \"has_avg\": %s,\n"
            "      \"running_avg\": %.2f,\n"
            "      \"state\": %d,\n"
            "      \"state_text\": \"%s\",\n"
            "      \"connected\": %s,\n"
            "      \"last_update\": %ld\n"
            "    }%s\n",
            s->id, s->room, type_name, s->type, s->last_value, unit,
            s->has_avg ? "true" : "false", s->running_avg,
            (int)s->state, state_text,
            s->connected ? "true" : "false", (long)s->last_update,
            (i + 1 < snapshot.sensor_count) ? "," : "");
    }

    offset += snprintf(json + offset, 16384 - offset, "  ]\n}\n");

    send_http_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)offset);
    free(json);
}

/* Xử lý GET /api/log?n= */
static void handle_api_log(int client_fd, const char *log_path, const char *query) {
    int max_lines = 50;
    char filter_src[32] = {0};

    if (query) {
        const char *p = strstr(query, "n=");
        if (p) {
            int n = atoi(p + 2);
            if (n > 0 && n <= 500) max_lines = n;
        }
        const char *pf = strstr(query, "source=");
        if (!pf) pf = strstr(query, "filter=");
        if (pf) {
            sscanf(pf + ((pf[0]=='f') ? 7 : 7), "%31[^& \r\n]", filter_src);
        }
    }

    FILE *fp = fopen(log_path, "r");
    if (!fp) {
        const char *empty = "[]";
        send_http_response(client_fd, 200, "OK", "application/json", empty, strlen(empty));
        return;
    }

    /* Đọc toàn bộ file log thành mảng dòng */
    #define MAX_TOTAL_LOGS 1000
    char *lines[MAX_TOTAL_LOGS];
    int line_count = 0;
    char buf[512];

    while (fgets(buf, sizeof(buf), fp) && line_count < MAX_TOTAL_LOGS) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
            buf[--len] = '\0';
        }
        lines[line_count++] = strdup(buf);
    }
    fclose(fp);

    /* Lấy n dòng mới nhất từ dưới lên trên và lọc theo nguồn nếu có */
    char *json = (char *)malloc(65536);
    if (!json) {
        for (int i = 0; i < line_count; i++) free(lines[i]);
        return;
    }

    int offset = snprintf(json, 65536, "[\n");
    int written = 0;

    for (int i = line_count - 1; i >= 0 && written < max_lines && offset < 64000; i--) {
        char *line = lines[i];
        unsigned long seq = 0;
        char date[16] = {0}, time_str[16] = {0}, source[16] = {0};
        char msg[384] = {0};

        if (sscanf(line, "%lu %15s %15s %15s %[^\n]", &seq, date, time_str, source, msg) >= 5) {
            if (strlen(filter_src) > 0 && strcasecmp(filter_src, source) != 0 && strcasecmp(filter_src, "ALL") != 0) {
                continue;
            }

            char datetime[32];
            snprintf(datetime, sizeof(datetime), "%s %s", date, time_str);

            /* Thoát ký tự nháy kép trong msg nếu có */
            char esc_msg[512] = {0};
            size_t ep = 0;
            for (size_t k = 0; line[k] && ep < sizeof(esc_msg) - 2; k++) {
                if (msg[k] == '"' || msg[k] == '\\') {
                    esc_msg[ep++] = '\\';
                }
                esc_msg[ep++] = msg[k];
            }

            offset += snprintf(json + offset, 65536 - offset,
                "%s  {\"seq\": %lu, \"time\": \"%s\", \"source\": \"%s\", \"message\": \"%s\"}",
                (written > 0) ? ",\n" : "", seq, datetime, source, esc_msg);
            written++;
        }
    }

    offset += snprintf(json + offset, 65536 - offset, "\n]\n");

    send_http_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)offset);

    for (int i = 0; i < line_count; i++) free(lines[i]);
    free(json);
}

/* Xử lý GET /api/history?id=&n= */
static void handle_api_history(int client_fd, const char *db_path, const char *query) {
    int sensor_id = 10;
    int max_points = 50;

    if (query) {
        const char *p = strstr(query, "id=");
        if (p) sensor_id = atoi(p + 3);
        const char *pn = strstr(query, "n=");
        if (pn) {
            int n = atoi(pn + 2);
            if (n > 0 && n <= 500) max_points = n;
        }
    }

    sqlite3 *db;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        const char *err = "{\"error\":\"SQLite busy or unavailable\"}";
        send_http_response(client_fd, 500, "Database Error", "application/json", err, strlen(err));
        if (db) sqlite3_close(db);
        return;
    }

    sqlite3_busy_timeout(db, 1000);

    const char *sql = "SELECT value, ts FROM SensorData WHERE id = ? ORDER BY ts DESC LIMIT ?;";
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_close(db);
        const char *err = "{\"error\":\"Failed to prepare query\"}";
        send_http_response(client_fd, 500, "Database Error", "application/json", err, strlen(err));
        return;
    }

    sqlite3_bind_int(stmt, 1, sensor_id);
    sqlite3_bind_int(stmt, 2, max_points);

    char *json = (char *)malloc(32768);
    if (!json) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return;
    }

    int offset = snprintf(json, 32768, "{\n  \"id\": %d,\n  \"points\": [\n", sensor_id);
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && offset < 31000) {
        double val = sqlite3_column_double(stmt, 0);
        long long ts = sqlite3_column_int64(stmt, 1);

        offset += snprintf(json + offset, 32768 - offset,
            "%s    {\"value\": %.2f, \"ts\": %lld}",
            (count > 0) ? ",\n" : "", val, ts);
        count++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    offset += snprintf(json + offset, 32768 - offset, "\n  ]\n}\n");

    send_http_response(client_fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)offset);
    free(json);
}

/* Dispatch request HTTP */
static void handle_http_request(int client_fd, const char *req,
                                const char *shm_name, const char *log_path,
                                const char *db_path, const char *web_dir) {
    char method[16] = {0};
    char uri[256] = {0};

    if (sscanf(req, "%15s %255s", method, uri) < 2) {
        const char *bad = "400 Bad Request";
        send_http_response(client_fd, 400, "Bad Request", "text/plain", bad, strlen(bad));
        log_event("WEB", "Web server received an invalid request (status code 400).");
        return;
    }

    if (strcmp(method, "GET") != 0) {
        const char *err = "405 Method Not Allowed";
        send_http_response(client_fd, 405, "Method Not Allowed", "text/plain", err, strlen(err));
        log_event("WEB", "Web server received an invalid request (status code 405).");
        return;
    }

    char *query = strchr(uri, '?');
    if (query) {
        *query = '\0';
        query++;
    }

    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        handle_get_index(client_fd, web_dir);
    }
    else if (strcmp(uri, "/api/status") == 0) {
        gateway_status_t *status = shared_status_open(shm_name);
        handle_api_status(client_fd, status);
        if (status) shared_status_detach(status);
    }
    else if (strcmp(uri, "/api/log") == 0) {
        handle_api_log(client_fd, log_path, query);
    }
    else if (strcmp(uri, "/api/history") == 0) {
        handle_api_history(client_fd, db_path, query);
    }
    else {
        const char *nf = "404 Not Found";
        send_http_response(client_fd, 404, "Not Found", "text/plain", nf, strlen(nf));
        log_event("WEB", "Web server received an invalid request (status code 404).");
    }
}

/* Hàm chính của Web Process */
static void web_process_main(uint16_t http_port, const char *shm_name, const char *fifo_path,
                             const char *log_path, const char *db_path, const char *web_dir) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = web_sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Bỏ qua tín hiệu SIGPIPE khi client đột ngột đóng kết nối */
    signal(SIGPIPE, SIG_IGN);

    /* Khởi tạo phía client gửi log vào FIFO */
    log_client_init(fifo_path);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[WEB] socket that bai");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 127.0.0.1 */
    addr.sin_port = htons(http_port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[WEB] bind that bai");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 32) < 0) {
        perror("[WEB] listen that bai");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    log_event("WEB", "Web server started on port %u.", (unsigned int)http_port);

    struct pollfd pfd;
    pfd.fd = server_fd;
    pfd.events = POLLIN;

    while (!g_web_stop) {
        int ret = poll(&pfd, 1, 500);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) continue;

        if (pfd.revents & POLLIN) {
            struct sockaddr_in client_addr;
            socklen_t clen = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &clen);
            if (client_fd >= 0) {
                char req_buf[2048];
                ssize_t n = recv(client_fd, req_buf, sizeof(req_buf) - 1, 0);
                if (n > 0) {
                    req_buf[n] = '\0';
                    handle_http_request(client_fd, req_buf, shm_name, log_path, db_path, web_dir);
                }
                close(client_fd);
            }
        }
    }

    close(server_fd);
    log_client_close();
    exit(EXIT_SUCCESS);
}

pid_t web_server_start(uint16_t http_port, const char *shm_name, const char *fifo_path,
                       const char *log_path, const char *db_path, const char *web_dir) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[MAIN] fork web process that bai");
        return -1;
    }

    if (pid == 0) {
        web_process_main(http_port, shm_name, fifo_path, log_path, db_path, web_dir);
        exit(EXIT_SUCCESS);
    }

    return pid;
}


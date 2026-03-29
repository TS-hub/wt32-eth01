/* iperf3 server emulation for ESP32-S3 / W5500
 *
 * Wire protocol (iperf3 3.x):
 *  1. Client connects (ctrl socket), sends 37-byte cookie
 *  2. Client sends PARAM_EXCHANGE(9) + 4-byte-len + JSON params
 *  3. Server acks PARAM_EXCHANGE(9), sends CREATE_STREAMS(10)
 *  4. Client connects data stream(s), each sends the same cookie
 *  5. Server sends TEST_START(1), TEST_RUNNING(2)
 *  6. Client sends data; server counts bytes
 *  7. Client sends TEST_END(4) on ctrl; data sockets close
 *  8. Server sends EXCHANGE_RESULTS(13), both sides swap JSON stats
 *  9. Server sends DISPLAY_RESULTS(14), IPERF_DONE(15)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "cJSON.h"
#include "iperf3_server.h"

#define TAG "iperf3"

/* Protocol constants */
#define IPERF3_PORT           5201
#define IPERF3_COOKIE_SIZE    37
#define IPERF3_MAX_STREAMS    4
#define IPERF3_RX_BUFSIZE     (16 * 1024)

/* State machine values (signed byte sent on ctrl socket) */
#define ST_TEST_START       1
#define ST_TEST_RUNNING     2
#define ST_TEST_END         4
#define ST_PARAM_EXCHANGE   9
#define ST_CREATE_STREAMS   10
#define ST_CLIENT_TERMINATE 12
#define ST_EXCHANGE_RESULTS 13
#define ST_DISPLAY_RESULTS  14
#define ST_IPERF_DONE       15
#define ST_ACCESS_DENIED    (-1)
#define ST_SERVER_ERROR     (-2)

static volatile bool s_running = false;
static int s_listen_fd = -1;

/* ------------------------------------------------------------------ */

static void gen_cookie(char cookie[IPERF3_COOKIE_SIZE])
{
    static const char pool[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    for (int i = 0; i < IPERF3_COOKIE_SIZE - 1; i++) {
        cookie[i] = pool[esp_random() % (sizeof(pool) - 1)];
    }
    cookie[IPERF3_COOKIE_SIZE - 1] = '\0';
}

/* Receive exactly len bytes; returns len on success, -1 on error */
static int recv_exact(int fd, void *buf, int len)
{
    int done = 0;
    while (done < len) {
        int n = recv(fd, (uint8_t *)buf + done, len - done, 0);
        if (n <= 0) return -1;
        done += n;
    }
    return len;
}

static int send_state(int fd, int state)
{
    int8_t s = (int8_t)state;
    return (int)send(fd, &s, 1, 0);
}

static int recv_state(int fd, int8_t *out)
{
    return recv(fd, out, 1, 0);
}

/* Send 4-byte big-endian length followed by JSON text */
static int send_json(int fd, cJSON *json)
{
    char *str = cJSON_PrintUnformatted(json);
    if (!str) return -1;
    uint32_t len = (uint32_t)strlen(str);
    uint32_t net_len = htonl(len);
    int r = (int)send(fd, &net_len, 4, 0);
    if (r == 4) {
        r = (int)send(fd, str, (int)len, 0);
        if (r != (int)len) r = -1;
    } else {
        r = -1;
    }
    cJSON_free(str);
    return r;
}

/* Receive 4-byte length + JSON; caller must cJSON_Delete the result */
static cJSON *recv_json(int fd)
{
    uint32_t net_len;
    if (recv_exact(fd, &net_len, 4) != 4) return NULL;
    uint32_t len = ntohl(net_len);
    if (len == 0 || len > 65536) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    if (recv_exact(fd, buf, (int)len) != (int)len) { free(buf); return NULL; }
    buf[len] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static int64_t get_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* ------------------------------------------------------------------ */

static void iperf3_server_task(void *arg)
{
    char cookie[IPERF3_COOKIE_SIZE];
    char recv_cookie[IPERF3_COOKIE_SIZE];
    int  ctrl_fd = -1;
    int  data_fd[IPERF3_MAX_STREAMS];
    int  n_data = 0;
    uint8_t *buf = NULL;
    struct timeval tv;
    int8_t state;

    for (int i = 0; i < IPERF3_MAX_STREAMS; i++) data_fd[i] = -1;

    /* Create listening socket */
    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket: %s", strerror(errno));
        goto task_done;
    }
    {
        int opt = 1;
        setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }
    {
        struct sockaddr_in addr = {
            .sin_family      = AF_INET,
            .sin_port        = htons(IPERF3_PORT),
            .sin_addr.s_addr = INADDR_ANY,
        };
        if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "bind: %s", strerror(errno));
            goto task_done;
        }
    }
    if (listen(s_listen_fd, 5) < 0) {
        ESP_LOGE(TAG, "listen: %s", strerror(errno));
        goto task_done;
    }
    ESP_LOGI(TAG, "Listening on port %d", IPERF3_PORT);

    /* ============================================================
     * Main accept loop — handles one client at a time
     * ============================================================ */
    while (s_running) {
        struct sockaddr_in remote;
        socklen_t rlen = sizeof(remote);

        /* Non-blocking accept so we can check s_running */
        tv = (struct timeval){ .tv_sec = 2, .tv_usec = 0 };
        setsockopt(s_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ctrl_fd = accept(s_listen_fd, (struct sockaddr *)&remote, &rlen);
        if (ctrl_fd < 0) {
            if (!s_running) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            ESP_LOGW(TAG, "accept: %s", strerror(errno));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGI(TAG, "Client %s:%d", inet_ntoa(remote.sin_addr), ntohs(remote.sin_port));

        tv = (struct timeval){ .tv_sec = 15, .tv_usec = 0 };
        setsockopt(ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(ctrl_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        /* --- 1. Read cookie sent by client --- */
        if (recv_exact(ctrl_fd, cookie, IPERF3_COOKIE_SIZE) != IPERF3_COOKIE_SIZE) {
            ESP_LOGE(TAG, "cookie recv failed");
            goto close_session;
        }
        cookie[IPERF3_COOKIE_SIZE - 1] = '\0';
        ESP_LOGI(TAG, "Cookie: %.36s", cookie);

        /* --- 2. Send PARAM_EXCHANGE, receive JSON params from client --- */
        if (send_state(ctrl_fd, ST_PARAM_EXCHANGE) != 1) goto close_session;
        cJSON *params = recv_json(ctrl_fd);
        if (!params) { ESP_LOGE(TAG, "params JSON failed"); goto close_session; }

        /* Parse relevant fields */
        cJSON *j;
        j = cJSON_GetObjectItem(params, "parallel");
        int n_streams = (j && cJSON_IsNumber(j)) ? j->valueint : 1;
        if (n_streams < 1 || n_streams > IPERF3_MAX_STREAMS) n_streams = 1;

        j = cJSON_GetObjectItem(params, "time");
        int test_time = (j && cJSON_IsNumber(j)) ? j->valueint : 10;

        j = cJSON_GetObjectItem(params, "len");
        int blksize = (j && cJSON_IsNumber(j)) ? j->valueint : IPERF3_RX_BUFSIZE;
        if (blksize <= 0 || blksize > IPERF3_RX_BUFSIZE) blksize = IPERF3_RX_BUFSIZE;

        j = cJSON_GetObjectItem(params, "omit");
        int omit = (j && cJSON_IsNumber(j)) ? j->valueint : 0;

        j = cJSON_GetObjectItem(params, "interval");
        double interval = (j && cJSON_IsNumber(j)) ? j->valuedouble : 1.0;
        if (interval <= 0) interval = 1.0;

        bool reverse = false;
        j = cJSON_GetObjectItem(params, "reverse");
        if (j && (cJSON_IsTrue(j) || (cJSON_IsNumber(j) && j->valueint))) reverse = true;

        bool udp = false;
        j = cJSON_GetObjectItem(params, "udp");
        if (j && (cJSON_IsTrue(j) || (cJSON_IsNumber(j) && j->valueint))) udp = true;

        ESP_LOGI(TAG, "streams=%d time=%d blksize=%d udp=%d reverse=%d omit=%d interval=%.1f",
                 n_streams, test_time, blksize, udp, reverse, omit, interval);
        cJSON_Delete(params);

        /* reverse mode supported — server sends, client receives */
        if (udp) {
            ESP_LOGW(TAG, "UDP mode not supported");
            send_state(ctrl_fd, ST_ACCESS_DENIED);
            goto close_session;
        }

        /* --- 3. Send CREATE_STREAMS --- */
        if (send_state(ctrl_fd, ST_CREATE_STREAMS) != 1) goto close_session;

        /* --- 5. Accept data streams (same listen socket, identified by cookie) --- */
        tv = (struct timeval){ .tv_sec = 15, .tv_usec = 0 };
        setsockopt(s_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        n_data = 0;
        for (int i = 0; i < n_streams; i++) {
            data_fd[i] = accept(s_listen_fd, (struct sockaddr *)&remote, &rlen);
            if (data_fd[i] < 0) {
                ESP_LOGE(TAG, "data accept[%d]: %s", i, strerror(errno));
                goto close_session;
            }
            n_data++;
            if (recv_exact(data_fd[i], recv_cookie, IPERF3_COOKIE_SIZE) != IPERF3_COOKIE_SIZE) {
                ESP_LOGE(TAG, "data cookie recv[%d] failed", i);
                goto close_session;
            }
            if (memcmp(cookie, recv_cookie, IPERF3_COOKIE_SIZE) != 0) {
                ESP_LOGE(TAG, "cookie mismatch on stream %d", i);
                goto close_session;
            }
            ESP_LOGI(TAG, "Data stream %d accepted", i);
        }

        /* --- 6. TEST_START, TEST_RUNNING --- */
        if (send_state(ctrl_fd, ST_TEST_START)   != 1) goto close_session;
        if (send_state(ctrl_fd, ST_TEST_RUNNING) != 1) goto close_session;

        /* --- 7. Transfer data --- */
        buf = malloc(blksize);
        if (!buf) {
            send_state(ctrl_fd, ST_SERVER_ERROR);
            goto close_session;
        }

        /* Compute maxfd across ctrl + data sockets */
        int maxfd = ctrl_fd;
        for (int i = 0; i < n_data; i++) {
            if (data_fd[i] > maxfd) maxfd = data_fd[i];
        }
        maxfd++;

        uint64_t total_bytes    = 0;
        uint64_t interval_bytes = 0;
        int64_t  t_start        = get_us();
        int64_t  t_last_report  = t_start;
        int      streams_done   = 0;
        bool     ctrl_ended     = false;
        double   t_report_sec   = (omit > 0) ? -(double)omit : 0.0;

        if (omit > 0) printf("\nOmitting first %d second(s)...\n", omit);
        printf("\nInterval        Bandwidth\n");

        if (!reverse) {
            /* Normal mode: receive data from client */
            tv = (struct timeval){ .tv_sec = test_time + 10, .tv_usec = 0 };
            for (int i = 0; i < n_data; i++) {
                setsockopt(data_fd[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            }

            while (streams_done < n_data && !ctrl_ended) {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(ctrl_fd, &rfds);
                for (int i = 0; i < n_data; i++) {
                    if (data_fd[i] >= 0) FD_SET(data_fd[i], &rfds);
                }

                int64_t now   = get_us();
                int64_t wake  = t_last_report + (int64_t)(interval * 1e6);
                int64_t delay = wake - now;
                if (delay < 10000)   delay = 10000;
                if (delay > 2000000) delay = 2000000;
                tv.tv_sec  = (long)(delay / 1000000);
                tv.tv_usec = (long)(delay % 1000000);

                int sel = select(maxfd, &rfds, NULL, NULL, &tv);
                if (sel < 0) { if (errno == EINTR) continue; break; }

                now = get_us();
                if (now - t_last_report >= (int64_t)(interval * 1e6)) {
                    double elapsed = (now - t_start) / 1e6;
                    double int_dur = (now - t_last_report) / 1e6;
                    if (elapsed >= (double)omit && int_dur > 0) {
                        double bw = (interval_bytes * 8.0) / int_dur / 1e6;
                        double t0 = t_report_sec;
                        t_report_sec = elapsed - omit;
                        printf("%.1f-%.1f sec  %.2f Mbits/sec\n", t0, t_report_sec, bw);
                    }
                    interval_bytes = 0;
                    t_last_report  = now;
                }

                if (sel == 0) continue;

                if (FD_ISSET(ctrl_fd, &rfds)) {
                    if (recv_state(ctrl_fd, &state) == 1) {
                        if (state == ST_TEST_END || state == ST_CLIENT_TERMINATE) {
                            ESP_LOGI(TAG, "Ctrl state %d received", state);
                            ctrl_ended = true;
                        }
                    } else {
                        ctrl_ended = true;
                    }
                }

                for (int i = 0; i < n_data; i++) {
                    if (data_fd[i] < 0 || !FD_ISSET(data_fd[i], &rfds)) continue;
                    int n = recv(data_fd[i], buf, blksize, 0);
                    if (n <= 0) {
                        close(data_fd[i]); data_fd[i] = -1;
                        streams_done++;
                    } else {
                        total_bytes    += (uint64_t)n;
                        interval_bytes += (uint64_t)n;
                    }
                }
            }

        } else {
            /* Reverse mode: server sends data for test_time seconds */
            memset(buf, 0x5A, blksize);
            int64_t t_deadline = t_start + (int64_t)test_time * 1000000LL;

            while (!ctrl_ended) {
                int64_t now = get_us();
                if (now >= t_deadline) break;

                fd_set rfds, wfds;
                FD_ZERO(&rfds); FD_ZERO(&wfds);
                FD_SET(ctrl_fd, &rfds);
                bool any_open = false;
                for (int i = 0; i < n_data; i++) {
                    if (data_fd[i] >= 0) {
                        FD_SET(data_fd[i], &wfds);
                        any_open = true;
                    }
                }
                if (!any_open) break;

                int64_t remaining = t_deadline - now;
                if (remaining > 100000) remaining = 100000;
                tv.tv_sec  = 0;
                tv.tv_usec = (long)remaining;

                int sel = select(maxfd, &rfds, &wfds, NULL, &tv);
                if (sel < 0) { if (errno == EINTR) continue; break; }

                if (FD_ISSET(ctrl_fd, &rfds)) {
                    if (recv_state(ctrl_fd, &state) == 1 &&
                        (state == ST_CLIENT_TERMINATE || state == ST_TEST_END)) {
                        ctrl_ended = true; break;
                    }
                }

                for (int i = 0; i < n_data; i++) {
                    if (data_fd[i] < 0 || !FD_ISSET(data_fd[i], &wfds)) continue;
                    int n = send(data_fd[i], buf, blksize, 0);
                    if (n > 0) {
                        total_bytes    += (uint64_t)n;
                        interval_bytes += (uint64_t)n;
                    } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        ESP_LOGW(TAG, "send stream %d: %s", i, strerror(errno));
                        close(data_fd[i]); data_fd[i] = -1;
                    }
                }

                now = get_us();
                if (now - t_last_report >= (int64_t)(interval * 1e6)) {
                    double elapsed = (now - t_start) / 1e6;
                    double int_dur = (now - t_last_report) / 1e6;
                    if (elapsed >= (double)omit && int_dur > 0) {
                        double bw = (interval_bytes * 8.0) / int_dur / 1e6;
                        double t0 = t_report_sec;
                        t_report_sec = elapsed - omit;
                        printf("%.1f-%.1f sec  %.2f Mbits/sec\n", t0, t_report_sec, bw);
                    }
                    interval_bytes = 0;
                    t_last_report  = now;
                }
            }

            /* Close data sockets — signals EOF to client */
            for (int i = 0; i < n_data; i++) {
                if (data_fd[i] >= 0) { close(data_fd[i]); data_fd[i] = -1; }
            }
            n_data = 0;
        }

        int64_t t_end = get_us();
        uint64_t reported_bytes = total_bytes;

        /* Drain residual data (normal mode only) */
        if (!reverse) {
            tv = (struct timeval){ .tv_sec = 2, .tv_usec = 0 };
            for (int i = 0; i < n_data; i++) {
                if (data_fd[i] < 0) continue;
                setsockopt(data_fd[i], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                int n;
                while ((n = recv(data_fd[i], buf, blksize, 0)) > 0) {}
                close(data_fd[i]); data_fd[i] = -1;
            }
            n_data = 0;
        }

        double  dur   = (t_end - t_start) / 1e6;
        if (dur <= 0) dur = 0.001;
        double avg_bw = (reported_bytes * 8.0) / dur / 1e6;

        printf("0.0-%.1f sec  %.2f Mbits/sec  (average)\n", dur, avg_bw);
        printf("%s %" PRIu64 " bytes in %.2f sec = %.2f Mbits/sec\n",
               reverse ? "Sent" : "Received", reported_bytes, dur, avg_bw);

        free(buf); buf = NULL;

        /* Wait for TEST_END if not yet received */
        if (!ctrl_ended) {
            tv = (struct timeval){ .tv_sec = 5, .tv_usec = 0 };
            setsockopt(ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            recv_state(ctrl_fd, &state);
        }

        /* --- 8. Results exchange --- */
        tv = (struct timeval){ .tv_sec = 15, .tv_usec = 0 };
        setsockopt(ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(ctrl_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (send_state(ctrl_fd, ST_EXCHANGE_RESULTS) != 1) goto close_session;

        cJSON *cli_res = recv_json(ctrl_fd);

        /* Extract stream IDs from client results so we can mirror them back */
        int stream_ids[IPERF3_MAX_STREAMS];
        int n_ids = 0;
        if (cli_res) {
            cJSON *cli_streams = cJSON_GetObjectItem(cli_res, "streams");
            cJSON *entry;
            cJSON_ArrayForEach(entry, cli_streams) {
                if (n_ids < IPERF3_MAX_STREAMS) {
                    cJSON *id_j = cJSON_GetObjectItem(entry, "id");
                    stream_ids[n_ids] = (id_j && cJSON_IsNumber(id_j)) ? id_j->valueint : (n_ids + 1);
                    n_ids++;
                }
            }
            cJSON_Delete(cli_res);
        }
        /* Fall back to sequential IDs if client results were missing */
        for (int i = n_ids; i < n_streams; i++) stream_ids[i] = i + 1;

        cJSON *srv_res = cJSON_CreateObject();
        cJSON *streams_arr = cJSON_AddArrayToObject(srv_res, "streams");
        double bytes_per_stream = (double)reported_bytes / (n_streams > 0 ? n_streams : 1);
        for (int i = 0; i < n_streams; i++) {
            cJSON *si = cJSON_CreateObject();
            cJSON_AddNumberToObject(si, "id",          stream_ids[i]);
            cJSON_AddNumberToObject(si, "bytes",       bytes_per_stream);
            cJSON_AddNumberToObject(si, "retransmits", 0);
            cJSON_AddNumberToObject(si, "jitter",      0.0);
            cJSON_AddNumberToObject(si, "errors",      0);
            cJSON_AddNumberToObject(si, "packets",     0);
            cJSON_AddNumberToObject(si, "start_time",  0.0);
            cJSON_AddNumberToObject(si, "end_time",    dur);
            cJSON_AddItemToArray(streams_arr, si);
        }
        cJSON_AddNumberToObject(srv_res, "cpu_util_total",  0.0);
        cJSON_AddNumberToObject(srv_res, "cpu_util_user",   0.0);
        cJSON_AddNumberToObject(srv_res, "cpu_util_system", 0.0);
        cJSON_AddNumberToObject(srv_res, "sender_has_retransmits", 0);

        if (send_json(ctrl_fd, srv_res) < 0) ESP_LOGW(TAG, "send results failed");
        cJSON_Delete(srv_res);

        send_state(ctrl_fd, ST_DISPLAY_RESULTS);
        send_state(ctrl_fd, ST_IPERF_DONE);

close_session:
        for (int i = 0; i < IPERF3_MAX_STREAMS; i++) {
            if (data_fd[i] >= 0) { close(data_fd[i]); data_fd[i] = -1; }
        }
        n_data = 0;
        if (ctrl_fd >= 0) { close(ctrl_fd); ctrl_fd = -1; }
        if (buf) { free(buf); buf = NULL; }
        ESP_LOGI(TAG, "Session closed, waiting for next client");
    }

task_done:
    if (ctrl_fd >= 0) close(ctrl_fd);
    for (int i = 0; i < IPERF3_MAX_STREAMS; i++) {
        if (data_fd[i] >= 0) close(data_fd[i]);
    }
    if (s_listen_fd >= 0) { close(s_listen_fd); s_listen_fd = -1; }
    if (buf) free(buf);
    ESP_LOGI(TAG, "Task stopped");
    s_running = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */

esp_err_t iperf3_server_start(void)
{
    if (s_running) { ESP_LOGW(TAG, "already running"); return ESP_FAIL; }
    s_running = true;
    BaseType_t r = xTaskCreatePinnedToCore(
        iperf3_server_task, "iperf3_srv",
        12288, NULL, 5, NULL, 1);
    if (r != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t iperf3_server_stop(void)
{
    if (!s_running) return ESP_OK;
    s_running = false;
    if (s_listen_fd >= 0) {
        shutdown(s_listen_fd, SHUT_RDWR);
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Console command                                                      */
/* ------------------------------------------------------------------ */

static struct {
    struct arg_lit *server;
    struct arg_lit *abort;
    struct arg_end *end;
} iperf3_args;

static int cmd_iperf3(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&iperf3_args);
    if (nerrors) {
        arg_print_errors(stderr, iperf3_args.end, argv[0]);
        return 1;
    }
    if (iperf3_args.abort->count) {
        iperf3_server_stop();
        return 0;
    }
    if (iperf3_args.server->count) {
        if (iperf3_server_start() == ESP_OK) {
            printf("iperf3 server started on port %d\n", IPERF3_PORT);
        }
        return 0;
    }
    printf("Usage: iperf3 -s        start server\n");
    printf("       iperf3 --abort   stop server\n");
    return 0;
}

esp_err_t iperf3_register_cmd(void)
{
    iperf3_args.server = arg_lit0("s", "server", "run as iperf3 server (TCP receive, port 5201)");
    iperf3_args.abort  = arg_lit0(NULL, "abort", "stop running iperf3 server");
    iperf3_args.end    = arg_end(1);

    const esp_console_cmd_t cmd = {
        .command  = "iperf3",
        .help     = "iperf3-compatible TCP server (iperf3 -c <this-ip>)",
        .hint     = NULL,
        .func     = cmd_iperf3,
        .argtable = &iperf3_args,
    };
    return esp_console_cmd_register(&cmd);
}

// daemon/virt_sensord.c
//a simple userspace daemon that reads from the virt_sensor driver and serves data over HTTP + allows interval configuration via ioctl
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define VS_IOC_MAGIC 'v'
#define VS_IOC_SET_INTERVAL_MS _IOW(VS_IOC_MAGIC, 1, int)

static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

static int parse_temp_milli_c(const char *json_line) {
    const char *p = strstr(json_line, "temp_milli_c");
    if (!p) return INT_MIN;
    p = strchr(p, ':');
    if (!p) return INT_MIN;
    return atoi(p + 1);
}

static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    if (listen(fd, 16) < 0) { close(fd); return -1; }

    return fd;
}

static void http_send(int cfd, int code, const char *body) {
    const char *msg = (code == 200) ? "OK" : (code == 400 ? "Bad Request" : "Not Found");
    char hdr[256];
    int blen = (int)strlen(body);
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, msg, blen
    );
    (void)write(cfd, hdr, (size_t)n);
    (void)write(cfd, body, (size_t)blen);
}

static int extract_query_int(const char *req, const char *key, int *out) {
    // super tiny query parser: looks for "...?key=123"
    const char *q = strchr(req, '?');
    if (!q) return 0;
    q++; // after '?'

    // find key=
    size_t klen = strlen(key);
    const char *p = q;
    while ((p = strstr(p, key)) != NULL) {
        if (p == q || p[-1] == '&') {
            if (p[klen] == '=') {
                int v = atoi(p + (int)klen + 1);
                *out = v;
                return 1;
            }
        }
        p += klen;
    }
    return 0;
}

int main(void) {
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    const char *dev_path = "/dev/virt_sensor0";
    int devfd = open(dev_path, O_RDONLY | O_NONBLOCK);
    if (devfd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", dev_path, strerror(errno));
        return 1;
    }

    int lfd = make_listen_socket(8080);
    if (lfd < 0) {
        fprintf(stderr, "listen socket failed: %s\n", strerror(errno));
        close(devfd);
        return 1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        fprintf(stderr, "epoll_create1 failed: %s\n", strerror(errno));
        close(lfd);
        close(devfd);
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = 1; // device
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, devfd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl add devfd failed: %s\n", strerror(errno));
        close(epfd); close(lfd); close(devfd);
        return 1;
    }

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = 2; // listener
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev) < 0) {
        fprintf(stderr, "epoll_ctl add lfd failed: %s\n", strerror(errno));
        close(epfd); close(lfd); close(devfd);
        return 1;
    }

    enum { WIN = 50 };
    int buf[WIN];
    int count = 0, idx = 0;
    long long sum = 0;

    int current = 0;

    fprintf(stderr, "virt_sensord: epoll on %s + http://127.0.0.1:8080\n", dev_path);

    while (!g_stop) {
        struct epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "epoll_wait failed: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.u32 == 1) {
                // device readable (driver's poll() says new sample ready)
                char line[128];
                // read the latest line
                ssize_t rn = read(devfd, line, sizeof(line) - 1);
                if (rn > 0) {
                    line[rn] = '\0';
                    int v = parse_temp_milli_c(line);
                    if (v != INT_MIN) {
                        current = v;
                        if (count < WIN) {
                            buf[count++] = v;
                            sum += v;
                        } else {
                            sum -= buf[idx];
                            buf[idx] = v;
                            sum += v;
                            idx = (idx + 1) % WIN;
                        }
                    }
                }
            } else if (events[i].data.u32 == 2) {
                // incoming connection
                int cfd = accept(lfd, NULL, NULL);
                if (cfd < 0) continue;

                char req[1024];
                ssize_t rn = read(cfd, req, sizeof(req) - 1);
                if (rn < 0) rn = 0;
                req[rn] = '\0';

                if (strncmp(req, "GET /current", 12) == 0) {
                    char body[128];
                    snprintf(body, sizeof(body),
                             "{\"temp_milli_c\":%d}\n", current);
                    http_send(cfd, 200, body);

                } else if (strncmp(req, "GET /stats", 10) == 0) {
                    int avg = (count > 0) ? (int)(sum / count) : 0;
                    char body[192];
                    snprintf(body, sizeof(body),
                             "{\"window\":%d,\"count\":%d,\"avg_temp_milli_c\":%d,\"current_temp_milli_c\":%d}\n",
                             WIN, count, avg, current);
                    http_send(cfd, 200, body);

                } else if (strncmp(req, "GET /config", 11) == 0) {
                    int new_ms;
                    if (!extract_query_int(req, "interval_ms", &new_ms)) {
                        http_send(cfd, 400, "{\"error\":\"missing interval_ms\"}\n");
                    } else {
                        int rc = ioctl(devfd, VS_IOC_SET_INTERVAL_MS, &new_ms);
                        if (rc < 0) {
                            char body[256];
                            snprintf(body, sizeof(body),
                                     "{\"error\":\"ioctl failed\",\"errno\":%d}\n", errno);
                            http_send(cfd, 400, body);
                        } else {
                            char body[128];
                            snprintf(body, sizeof(body),
                                     "{\"ok\":true,\"interval_ms\":%d}\n", new_ms);
                            http_send(cfd, 200, body);
                        }
                    }

                } else {
                    http_send(cfd, 404, "{\"error\":\"not found\"}\n");
                }

                close(cfd);
            }
        }
    }

    close(epfd);
    close(lfd);
    close(devfd);
    fprintf(stderr, "virt_sensord: exiting\n");
    return 0;
} 
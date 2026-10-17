// Gemi3DS - servidor HTTP mínimo para vincular desde el teléfono
#include "i18n.h"
#include "pair.h"
#include "util.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fd = -1;
static int s_port;
static char s_url[64];
static char *s_html; static size_t s_html_len;
static volatile int s_state = PAIR_WAITING;
static char s_msg[256];
static char s_key[128], s_name[48];
static bool s_new, s_seen;

bool pair_running(void) { return s_fd >= 0; }
const char *pair_url(void) { return s_url; }
bool pair_phone_seen(void) { return s_seen; }

bool pair_start(void) {
    if (s_fd >= 0) return true;
    if (!s_html) s_html = (char *)file_read("romfs:/pair.html", &s_html_len);
    s_state = PAIR_WAITING; s_msg[0] = 0; s_new = false; s_seen = false;
    u32 ip = (u32)gethostid();
    s_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_fd < 0) { snprintf(s_url, sizeof(s_url), _("(sin red)")); return false; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    int ok = -1;
    for (s_port = 8080; s_port < 8090; s_port++) {
        a.sin_port = htons(s_port);
        if ((ok = bind(s_fd, (struct sockaddr *)&a, sizeof(a))) == 0) break;
    }
    if (ok != 0 || listen(s_fd, 4) != 0) {
        close(s_fd); s_fd = -1;
        snprintf(s_url, sizeof(s_url), _("(sin red)"));
        return false;
    }
    fcntl(s_fd, F_SETFL, fcntl(s_fd, F_GETFL, 0) | O_NONBLOCK);
    struct in_addr ia; ia.s_addr = ip;
    if (ip == 0) snprintf(s_url, sizeof(s_url), _("(conéctate a Wi-Fi)"));
    else if (s_port == 80) snprintf(s_url, sizeof(s_url), "http://%s/", inet_ntoa(ia));
    else snprintf(s_url, sizeof(s_url), "http://%s:%d/", inet_ntoa(ia), s_port);
    return true;
}

void pair_stop(void) {
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
}

void pair_set_status(int state, const char *msg) {
    s_state = state;
    snprintf(s_msg, sizeof(s_msg), "%s", msg ? msg : "");
}

bool pair_take(char *key, int kn, char *name, int nn) {
    if (!s_new) return false;
    s_new = false;
    snprintf(key, kn, "%s", s_key);
    snprintf(name, nn, "%s", s_name);
    return true;
}

static void send_all(int fd, const char *d, size_t n) {
    size_t off = 0;
    int tries = 0;
    while (off < n && tries < 200) {
        int r = send(fd, d + off, n - off > 8192 ? 8192 : n - off, 0);
        if (r > 0) { off += r; tries = 0; }
        else {
            if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            struct pollfd p = {fd, POLLOUT, 0};
            poll(&p, 1, 50);
            tries++;
        }
    }
}

static void respond(int fd, int code, const char *type, const char *body, size_t len) {
    char h[300];
    int hl = snprintf(h, sizeof(h),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
        code, code == 200 ? "OK" : (code == 404 ? "Not Found" : "Bad Request"), type, (unsigned)len);
    send_all(fd, h, hl);
    if (len) send_all(fd, body, len);
}

static void json_escape(const char *in, char *out, int n) {
    int o = 0;
    for (; *in && o < n - 7; in++) {
        if (*in == '"' || *in == '\\') { out[o++] = '\\'; out[o++] = *in; }
        else if ((u8)*in < 0x20) { out[o++] = ' '; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static void form_get(const char *body, const char *k, char *out, int n) {
    out[0] = 0;
    size_t kl = strlen(k);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, k, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            const char *e = strchr(v, '&');
            int l = e ? (int)(e - v) : (int)strlen(v);
            if (l > n - 1) l = n - 1;
            memcpy(out, v, l); out[l] = 0;
            url_decode(out);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static void handle(int fd) {
    char buf[4096];
    int len = 0;
    int clen = -1; char *body = NULL;
    u64 t0 = osGetTime();
    while (len < (int)sizeof(buf) - 1 && osGetTime() - t0 < 2500) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 100) <= 0) continue;
        int r = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
        if (r <= 0) break;
        len += r; buf[len] = 0;
        char *he = strstr(buf, "\r\n\r\n");
        if (he) {
            body = he + 4;
            if (clen < 0) {
                char *cl = strstr(buf, "Content-Length:");
                if (!cl) cl = strstr(buf, "content-length:");
                clen = cl ? atoi(cl + 15) : 0;
            }
            if ((int)(len - (body - buf)) >= clen) break;
        }
    }
    buf[len] = 0;
    if (!strncmp(buf, "GET / ", 6) || !strncmp(buf, "GET /index", 10)) {
        s_seen = true;
        if (s_html) respond(fd, 200, "text/html; charset=utf-8", s_html, s_html_len);
        else respond(fd, 200, "text/plain; charset=utf-8", "Gemi3DS", 7);
    } else if (!strncmp(buf, "GET /status", 11)) {
        char esc[300], j[400];
        json_escape(s_msg, esc, sizeof(esc));
        int l = snprintf(j, sizeof(j), "{\"s\":%d,\"m\":\"%s\",\"l\":\"%s\"}", s_state, esc,
                         g_lang == LANG_JA ? "ja" : (g_lang == LANG_EN ? "en" : "es"));
        respond(fd, 200, "application/json", j, l);
    } else if (!strncmp(buf, "POST /pair", 10) && body) {
        char k[160], n[64];
        form_get(body, "key", k, sizeof(k));
        form_get(body, "name", n, sizeof(n));
        str_trim(k); str_trim(n);
        if (strlen(k) >= 20 && strlen(k) < sizeof(s_key)) {
            snprintf(s_key, sizeof(s_key), "%s", k);
            snprintf(s_name, sizeof(s_name), "%s", n);
            s_new = true;
            s_state = PAIR_CHECKING;
            respond(fd, 200, "application/json", "{\"ok\":true}", 11);
        } else {
            respond(fd, 400, "application/json", "{\"ok\":false}", 12);
        }
    } else {
        respond(fd, 404, "text/plain", "", 0);
    }
}

void pair_poll(void) {
    if (s_fd < 0) return;
    for (int i = 0; i < 4; i++) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int c = accept(s_fd, (struct sockaddr *)&ca, &cl);
        if (c < 0) break;
        fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
        handle(c);
        shutdown(c, SHUT_RDWR);
        close(c);
    }
}

/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - servidor HTTP mínimo para vincular desde el teléfono y pasarle mensajes
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
static bool s_pairing;               // false: el servidor solo sirve mensajes compartidos

// mensajes compartidos: el QR lleva una URL corta y el texto completo se sirve desde aquí
#define SHARE_MAX 6
static struct { char id[8]; char *text; } s_share[SHARE_MAX];
static int s_share_next;

bool pair_running(void) { return s_fd >= 0; }
const char *pair_url(void) { return s_url; }
bool pair_phone_seen(void) { return s_seen; }

static bool server_open(void) {
    if (s_fd >= 0) return true;
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

bool pair_start(void) {
    if (!s_html) s_html = (char *)file_read("romfs:/pair.html", &s_html_len);
    s_state = PAIR_WAITING; s_msg[0] = 0; s_new = false; s_seen = false;
    s_pairing = true;
    return server_open();
}

void pair_stop(void) {
    if (s_fd >= 0) close(s_fd);
    s_fd = -1;
    s_pairing = false;
}

bool pair_share(const char *text, char *url, int n) {
    if (!server_open() || strncmp(s_url, "http", 4)) return false;
    int k = s_share_next++ % SHARE_MAX;
    free(s_share[k].text);
    s_share[k].text = strdup(text);
    if (!s_share[k].text) return false;
    static const char abc[] = "abcdefghijkmnpqrstuvwxyz23456789";
    u64 r = svcGetSystemTick() ^ ((u64)osGetTime() << 16) ^ (u64)k * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 5; i++) { s_share[k].id[i] = abc[r % 32]; r = r / 32 * 2654435761u + 7; }
    s_share[k].id[5] = 0;
    snprintf(url, n, "%sm/%s", s_url, s_share[k].id);
    return true;
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

static void html_escape_append(char **o, size_t *len, size_t *cap, const char *in) {
    for (; *in; in++) {
        const char *rep = NULL; char one[2] = {*in, 0};
        if (*in == '<') rep = "&lt;"; else if (*in == '>') rep = "&gt;"; else if (*in == '&') rep = "&amp;";
        else if (*in == '"') rep = "&quot;"; else rep = one;
        size_t rl = strlen(rep);
        if (*len + rl + 1 > *cap) { *cap = (*cap + rl) * 2; *o = realloc(*o, *cap); }
        memcpy(*o + *len, rep, rl); *len += rl; (*o)[*len] = 0;
    }
}

// página para el teléfono con el texto completo y un botón para copiarlo
static void serve_share(int fd, const char *id) {
    const char *text = NULL;
    for (int i = 0; i < SHARE_MAX; i++)
        if (s_share[i].text && !strncmp(id, s_share[i].id, 5) && (id[5] == 0 || id[5] == ' ' || id[5] == '?')) text = s_share[i].text;
    if (!text) {
        const char *m = _("Este mensaje ya no está disponible. Vuelve a compartirlo desde la consola.");
        respond(fd, 404, "text/plain; charset=utf-8", m, strlen(m));
        return;
    }
    size_t cap = strlen(text) * 2 + 4096, len = 0;
    char *o = malloc(cap); o[0] = 0;
    char head[2600];
    snprintf(head, sizeof(head),
        "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>Gemi3DS</title><style>"
        ":root{color-scheme:light dark;--bg:#f8fafd;--card:#fff;--tx:#1f1f1f;--tx2:#5f6368;--ac:#0b57d0}"
        "@media(prefers-color-scheme:dark){:root{--bg:#131314;--card:#1e1f20;--tx:#e3e3e3;--tx2:#a8abb0;--ac:#a8c7fa}}"
        "body{margin:0;background:var(--bg);color:var(--tx);font:16px/1.5 system-ui,-apple-system,Roboto,sans-serif}"
        "main{max-width:640px;margin:0 auto;padding:20px 16px 96px}"
        "h1{font-size:20px;margin:4px 0 2px;background:linear-gradient(90deg,#4285f4,#9b72cb,#d96570);"
        "-webkit-background-clip:text;background-clip:text;color:transparent}"
        "p.s{color:var(--tx2);margin:0 0 16px;font-size:14px}"
        "#t{white-space:pre-wrap;word-wrap:break-word;background:var(--card);border-radius:16px;padding:16px;"
        "-webkit-user-select:text;user-select:text}"
        "button{position:fixed;left:50%%;bottom:20px;transform:translateX(-50%%);border:0;border-radius:24px;"
        "padding:14px 28px;font:600 16px system-ui,sans-serif;background:var(--ac);color:var(--bg);"
        "box-shadow:0 4px 16px #0004}"
        "textarea{position:fixed;opacity:0;top:0;left:0;width:1px;height:1px}"
        "</style></head><body><main><h1>Gemi3DS</h1><p class=s>%s</p><div id=t>",
        _("Mensaje compartido desde tu 3DS"));
    size_t hl = strlen(head);
    if (hl + 1 > cap) { cap = hl * 2; o = realloc(o, cap); }
    memcpy(o, head, hl + 1); len = hl;
    html_escape_append(&o, &len, &cap, text);
    char tail[1400];
    snprintf(tail, sizeof(tail),
        "</div></main><textarea id=c readonly></textarea><button id=b>%s</button><script>"
        "var b=document.getElementById('b'),t=document.getElementById('t').innerText;"
        "b.onclick=function(){var ok=function(){b.textContent='%s';setTimeout(function(){b.textContent='%s'},1800)};"
        "if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(t).then(ok);return}"
        "var c=document.getElementById('c');c.value=t;c.focus();c.select();c.setSelectionRange(0,t.length);"
        "try{document.execCommand('copy');ok()}catch(e){}}"
        "</script></body></html>",
        _("Copiar texto"), _("Copiado"), _("Copiar texto"));
    size_t tl = strlen(tail);
    if (len + tl + 1 > cap) { cap = len + tl + 1; o = realloc(o, cap); }
    memcpy(o + len, tail, tl + 1); len += tl;
    respond(fd, 200, "text/html; charset=utf-8", o, len);
    free(o);
}

static void handle(int fd) {
    char buf[4096];
    int len = 0;
    int clen = -1; char *body = NULL;
    u64 t0 = osGetTime();
    while (len < (int)sizeof(buf) - 1 && osGetTime() - t0 < 2500) {
        if (len == 0 && osGetTime() - t0 > 400) break;   // conexión previa del navegador sin petición: no bloquea la app
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
    if (!strncmp(buf, "GET /m/", 7)) {
        serve_share(fd, buf + 7);
    } else if (!s_pairing) {
        respond(fd, 404, "text/plain", "", 0);   // fuera de la vinculación no se aceptan claves
    } else if (!strncmp(buf, "GET / ", 6) || !strncmp(buf, "GET /index", 10)) {
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

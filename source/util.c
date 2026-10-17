// Gemi3DS - utilidades
#include "util.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <ctype.h>

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *b64_encode(const u8 *d, size_t len, size_t *out_len) {
    size_t ol = 4 * ((len + 2) / 3);
    char *o = malloc(ol + 1);
    if (!o) return NULL;
    size_t i = 0, j = 0;
    while (i + 2 < len) {
        u32 v = (d[i] << 16) | (d[i + 1] << 8) | d[i + 2];
        o[j++] = B64[(v >> 18) & 63]; o[j++] = B64[(v >> 12) & 63];
        o[j++] = B64[(v >> 6) & 63];  o[j++] = B64[v & 63];
        i += 3;
    }
    if (i < len) {
        u32 v = d[i] << 16;
        if (i + 1 < len) v |= d[i + 1] << 8;
        o[j++] = B64[(v >> 18) & 63]; o[j++] = B64[(v >> 12) & 63];
        o[j++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
        o[j++] = '=';
    }
    o[j] = 0;
    if (out_len) *out_len = j;
    return o;
}

static int b64v(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

u8 *b64_decode(const char *s, size_t len, size_t *out_len) {
    u8 *o = malloc(len / 4 * 3 + 4);
    if (!o) return NULL;
    u32 acc = 0; int bits = 0; size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64v((u8)s[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v; bits += 6;
        if (bits >= 8) { bits -= 8; o[j++] = (acc >> bits) & 0xFF; }
    }
    if (out_len) *out_len = j;
    return o;
}

u8 *file_read(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    u8 *b = malloc(n + 1);
    if (!b) { fclose(f); return NULL; }
    size_t r = fread(b, 1, n, f);
    fclose(f);
    b[r] = 0;
    if (size) *size = r;
    return b;
}

bool file_write(const char *path, const void *data, size_t size) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    size_t w = fwrite(data, 1, size, f);
    fclose(f);
    if (w != size) { remove(tmp); return false; }
    remove(path);
    if (rename(tmp, path) != 0) {
        // si rename falla, escribir directo
        f = fopen(path, "wb");
        if (!f) return false;
        fwrite(data, 1, size, f);
        fclose(f);
        remove(tmp);
    }
    return true;
}

void mkdirs(const char *path) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 6; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(buf, 0777); *p = '/'; }
    }
    mkdir(buf, 0777);
}

bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

void make_id(char *out, int n) {
    static u32 ctr;
    snprintf(out, n, "%llx%02lx", (unsigned long long)osGetTime(), (unsigned long)(ctr++ & 0xFF));
}

static const char *MESES[] = {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic"};

void fmt_when(u64 ms, char *out, int n) {
    // osGetTime: ms desde 1900; time(): s desde 1970
    time_t t = (time_t)(ms / 1000 - 2208988800ULL);
    time_t now = time(NULL);
    struct tm a, b;
    gmtime_r(&t, &a);
    gmtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) snprintf(out, n, _("Hoy %02d:%02d"), a.tm_hour, a.tm_min);
    else if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday - 1) snprintf(out, n, _("Ayer"));
    else if (g_lang == LANG_JA) snprintf(out, n, "%d月%d日", a.tm_mon + 1, a.tm_mday);
    else if (g_lang == LANG_EN) {
        static const char *MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        snprintf(out, n, "%s %d", MON[a.tm_mon % 12], a.tm_mday);
    } else snprintf(out, n, "%d %s", a.tm_mday, MESES[a.tm_mon % 12]);
}

void str_trim(char *s) {
    char *p = s;
    while (*p && isspace((u8)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int l = strlen(s);
    while (l > 0 && isspace((u8)s[l - 1])) s[--l] = 0;
}

void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && isxdigit((u8)s[1]) && isxdigit((u8)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else *o++ = *s++;
    }
    *o = 0;
}

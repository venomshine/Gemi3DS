/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - uso de los modelos
//
// La API de Gemini no dice cuánto cupo queda, así que se cuenta aquí: peticiones y tokens de
// cada modelo en el día del cupo (el gratuito se reinicia a medianoche, hora del Pacífico) y
// los límites que Google indica cuando rechaza una petición (429, "quotaValue").
#include <time.h>
#include "usage.h"
#include "app.h"
#include "util.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define USAGE_FILE APP_DIR "/usage.json"
#define USAGE_MAX 24

static UsageModel s_u[USAGE_MAX];
static int s_n;
static long long s_off;        // segundos UTC - reloj de la consola
static u32 s_keyfp;            // huella de la clave con la que se aprendieron los límites
static bool s_have_off;

// ------------------------------------------------------------ fechas
static long long days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

// horario de verano de EE. UU.: del 2º domingo de marzo (10:00 UTC) al 1er domingo de noviembre (09:00 UTC)
static int pacific_offset(long long utc) {
    time_t t = (time_t)utc;
    struct tm g; gmtime_r(&t, &g);
    int y = g.tm_year + 1900;
    long long mar1 = days_from_civil(y, 3, 1), nov1 = days_from_civil(y, 11, 1);
    int wmar = (int)((mar1 + 4) % 7), wnov = (int)((nov1 + 4) % 7);     // 0 = domingo
    long long dst_on = (mar1 + (7 - wmar) % 7 + 7) * 86400 + 10 * 3600;
    long long dst_off = (nov1 + (7 - wnov) % 7) * 86400 + 9 * 3600;
    return (utc >= dst_on && utc < dst_off) ? -7 * 3600 : -8 * 3600;
}

static int day_key(void) {
    time_t now = time(NULL);
    struct tm d;
    if (s_have_off) {
        long long utc = (long long)now + s_off;
        time_t pac = (time_t)(utc + pacific_offset(utc));
        gmtime_r(&pac, &d);
    } else localtime_r(&now, &d);
    return (d.tm_year + 1900) * 10000 + (d.tm_mon + 1) * 100 + d.tm_mday;
}

bool usage_reset_local(int *hh, int *mm) {
    if (!s_have_off) return false;
    long long utc = (long long)time(NULL) + s_off;
    int po = pacific_offset(utc);
    long long pac = utc + po;
    long long next_utc = (pac / 86400 + 1) * 86400 - po;
    time_t local = (time_t)(next_utc - s_off);
    struct tm d; localtime_r(&local, &d);
    *hh = d.tm_hour; *mm = d.tm_min;
    return true;
}

void usage_set_utc(time_t utc) {
    long long off = (long long)utc - (long long)time(NULL);
    // se ignoran saltos pequeños (latencia); se guarda con el resto al registrar la petición
    if (!s_have_off || llabs(off - s_off) > 30) { s_off = off; s_have_off = true; }
}

// ------------------------------------------------------------ guardar / cargar
static void usage_save(void) {
    cJSON *j = cJSON_CreateObject();
    if (s_have_off) cJSON_AddNumberToObject(j, "off", (double)s_off);
    cJSON_AddNumberToObject(j, "key", (double)s_keyfp);
    cJSON *a = cJSON_AddArrayToObject(j, "models");
    for (int i = 0; i < s_n; i++) {
        UsageModel *u = &s_u[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", u->id);
        cJSON_AddNumberToObject(o, "day", u->day);
        cJSON_AddNumberToObject(o, "req", u->req);
        cJSON_AddNumberToObject(o, "ok", u->ok);
        cJSON_AddNumberToObject(o, "lim", u->limited);
        cJSON_AddNumberToObject(o, "tin", u->tok_in);
        cJSON_AddNumberToObject(o, "tout", u->tok_out);
        cJSON_AddNumberToObject(o, "ld", u->limit_day);
        cJSON_AddNumberToObject(o, "lm", u->limit_min);
        cJSON_AddNumberToObject(o, "code", u->last_code);
        cJSON_AddItemToArray(a, o);
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (s) { file_write(USAGE_FILE, s, strlen(s)); free(s); }
}

static int jint(cJSON *o, const char *k) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}

void usage_load(void) {
    s_n = 0;
    char *raw = (char *)file_read(USAGE_FILE, NULL);
    if (!raw) return;
    cJSON *j = cJSON_Parse(raw);
    free(raw);
    if (!j) return;
    cJSON *off = cJSON_GetObjectItem(j, "off"), *o;
    if (cJSON_IsNumber(off)) { s_off = (long long)off->valuedouble; s_have_off = true; }
    cJSON *kf = cJSON_GetObjectItem(j, "key");
    if (cJSON_IsNumber(kf)) s_keyfp = (u32)kf->valuedouble;
    cJSON_ArrayForEach(o, cJSON_GetObjectItem(j, "models")) {
        cJSON *id = cJSON_GetObjectItem(o, "id");
        if (!cJSON_IsString(id) || s_n >= USAGE_MAX) continue;
        UsageModel *u = &s_u[s_n++];
        memset(u, 0, sizeof(*u));
        snprintf(u->id, sizeof(u->id), "%s", id->valuestring);
        u->day = jint(o, "day"); u->req = jint(o, "req"); u->ok = jint(o, "ok"); u->limited = jint(o, "lim");
        u->tok_in = jint(o, "tin"); u->tok_out = jint(o, "tout");
        u->limit_day = jint(o, "ld"); u->limit_min = jint(o, "lm"); u->last_code = jint(o, "code");
    }
    cJSON_Delete(j);
}

// ------------------------------------------------------------ registro
// los límites son de cada cuenta/proyecto: con otra clave se olvidan los aprendidos
static u32 key_fingerprint(void) {
    u32 h = 2166136261u;
    for (const char *p = g_cfg.api_key; *p; p++) h = (h ^ (u8)*p) * 16777619u;
    return h ? h : 1;
}
static void check_key(void) {
    u32 fp = key_fingerprint();
    if (fp == s_keyfp) return;
    for (int i = 0; i < s_n; i++) s_u[i].limit_day = s_u[i].limit_min = 0;
    s_keyfp = fp;
}

UsageModel *usage_find(const char *model) {
    for (int i = 0; i < s_n; i++) if (!strcmp(s_u[i].id, model)) return &s_u[i];
    return NULL;
}

// al cambiar el día del cupo se ponen a cero los contadores (los límites aprendidos se conservan)
static void roll_day(UsageModel *u, int day) {
    if (u->day == day) return;
    u->day = day;
    u->req = u->ok = u->limited = u->tok_in = u->tok_out = 0;
}

// "quotaId": "GenerateRequestsPerDayPerProjectPerModel-FreeTier", "quotaValue": "20"
static void learn_limits(UsageModel *u, const char *resp) {
    if (!resp) return;
    const char *p = resp;
    while ((p = strstr(p, "\"quotaId\""))) {
        const char *q = strchr(p + 9, '"');
        if (!q) break;
        char qid[120] = "";
        sscanf(q + 1, "%119[^\"]", qid);
        const char *v = strstr(q, "\"quotaValue\"");
        const char *next = strstr(q, "\"quotaId\"");
        int val = 0;
        if (v && (!next || v < next)) { const char *vq = strchr(v + 12, '"'); if (vq) val = atoi(vq + 1); }
        if (val > 0 && !strncmp(qid, "GenerateRequestsPer", 19)) {
            if (strstr(qid, "PerDay")) u->limit_day = val;
            else if (strstr(qid, "PerMinute")) u->limit_min = val;
        }
        p = q + 1;
    }
}

void usage_record(const char *model, long http_code, int tok_in, int tok_out, int first_ms, const char *resp) {
    if (!model || !model[0]) return;
    check_key();
    UsageModel *u = usage_find(model);
    if (!u) {
        if (s_n < USAGE_MAX) u = &s_u[s_n++];
        else {                                      // se reemplaza el menos usado recientemente
            u = &s_u[0];
            for (int i = 1; i < s_n; i++) if (s_u[i].last_time < u->last_time) u = &s_u[i];
        }
        memset(u, 0, sizeof(*u));
        snprintf(u->id, sizeof(u->id), "%s", model);
    }
    roll_day(u, day_key());
    u->req++;
    if (http_code == 200) u->ok++;
    if (http_code == 429) { u->limited++; learn_limits(u, resp); }
    // si se pasa del límite guardado y Google sigue respondiendo, el límite subió: se olvida hasta el próximo aviso
    if (http_code == 200 && u->limit_day && u->req > u->limit_day) u->limit_day = 0;
    u->tok_in += tok_in; u->tok_out += tok_out;
    u->last_code = (int)http_code;
    u->last_ms = first_ms;
    u->last_time = osGetTime();
    memmove(u->recent + 1, u->recent, sizeof(u->recent) - sizeof(u->recent[0]));
    u->recent[0] = u->last_time;
    usage_save();
}

float usage_left(const UsageModel *u) {
    if (!u->limit_day) return -1;
    float f = 1.0f - (float)u->req / u->limit_day + 1e-6f;   // 1e-6: que 0 peticiones dé el 100 % exacto
    return f < 0 ? 0 : f;
}

// ------------------------------------------------------------ consulta
int usage_rpm(const UsageModel *u) {
    u64 now = osGetTime();
    int n = 0;
    for (int i = 0; i < USAGE_RECENT; i++) if (u->recent[i] && now - u->recent[i] < 60000) n++;
    return n;
}

static int cmp_req(const void *a, const void *b) {
    const UsageModel *x = a, *y = b;
    if (x->req != y->req) return y->req - x->req;
    return x->last_time < y->last_time ? 1 : (x->last_time > y->last_time ? -1 : 0);
}

int usage_count(void) {
    check_key();
    int day = day_key();
    for (int i = 0; i < s_n; i++) roll_day(&s_u[i], day);
    qsort(s_u, s_n, sizeof(UsageModel), cmp_req);
    return s_n;
}

UsageModel *usage_get(int i) { return (i >= 0 && i < s_n) ? &s_u[i] : NULL; }

void usage_totals(int *req, int *tokens) {
    int day = day_key();
    *req = 0; *tokens = 0;
    for (int i = 0; i < s_n; i++) {
        roll_day(&s_u[i], day);
        *req += s_u[i].req;
        *tokens += s_u[i].tok_in + s_u[i].tok_out;
    }
}

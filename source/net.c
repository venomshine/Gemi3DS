/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - red
#include "i18n.h"
#include "net.h"
#include <curl/curl.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>

#define SOC_ALIGN 0x1000
#define SOC_SIZE  0x100000
#define QMAX 16

static u32 *s_socbuf;
static Thread s_thread;
static volatile bool s_run;
static LightEvent s_ev;
static LightLock s_qlock;
static NetReq *s_q[QMAX];
static int s_qh, s_qt;
static bool s_inited;

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    NetReq *r = ud;
    size_t n = size * nmemb;
    if (r->cancel) return 0;
    if (!r->t_first) r->t_first = osGetTime();
    long code = 0;
    // si hay callback y la respuesta es 200 -> streaming; si no, acumular
    if (r->on_data && r->http_code == 200) {
        r->on_data(r, ptr, n);
        return n;
    }
    if (r->resp_len + n + 1 > r->resp_cap) {
        size_t nc = r->resp_cap ? r->resp_cap * 2 : 16384;
        while (nc < r->resp_len + n + 1) nc *= 2;
        char *nb = realloc(r->resp, nc);
        if (!nb) return 0;
        r->resp = nb; r->resp_cap = nc;
    }
    memcpy(r->resp + r->resp_len, ptr, n);
    r->resp_len += n;
    r->resp[r->resp_len] = 0;
    (void)code;
    return n;
}

static size_t header_cb(char *buf, size_t size, size_t nitems, void *ud) {
    NetReq *r = ud;
    size_t n = size * nitems;
    if (n > 9 && !strncmp(buf, "HTTP/", 5)) {
        const char *sp = memchr(buf, ' ', n);
        if (sp) r->http_code = strtol(sp + 1, NULL, 10);
    }
    return n;
}

static int progress_cb(void *ud, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
    NetReq *r = ud;
    if (r->cancel) return 1;
    if (r->first_byte_timeout_ms > 0 && !r->t_first && osGetTime() - r->t_start > (u64)r->first_byte_timeout_ms) {
        r->fb_timed_out = true;
        return 1;
    }
    return 0;
}

static bool s_full_ca;   // usar el paquete completo de CA (si las raíces de Google fallan)

static void perform_once(CURL *c, NetReq *r);

static void perform(CURL *c, NetReq *r) {
    perform_once(c, r);
    if ((r->curl_code == CURLE_PEER_FAILED_VERIFICATION || r->curl_code == CURLE_SSL_CACERT_BADFILE) && !s_full_ca && !r->cancel) {
        s_full_ca = true;   // otra cadena de certificados: reintentar con todas las raíces
        r->resp_len = 0; r->err[0] = 0;
        perform_once(c, r);
    }
}

static void perform_once(CURL *c, NetReq *r) {
    struct curl_slist *h = NULL;
    char kh[200];
    curl_easy_reset(c);
    curl_easy_setopt(c, CURLOPT_URL, r->url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "Gemi3DS/1.0 (Nintendo 3DS)");
    curl_easy_setopt(c, CURLOPT_CAINFO, s_full_ca ? "romfs:/cacert.pem" : "romfs:/google_roots.pem");
    // no reutilizar conexiones que llevan rato inactivas: Google/el router las cortan en silencio
    // y la petición se quedaría esperando minutos sobre un socket muerto
    curl_easy_setopt(c, CURLOPT_MAXAGE_CONN, 15L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 25L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 90L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 32768L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, r);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, r);
    if (r->api_key[0]) {
        snprintf(kh, sizeof(kh), "x-goog-api-key: %s", r->api_key);
        h = curl_slist_append(h, kh);
    }
    if (r->body) {
        h = curl_slist_append(h, "Content-Type: application/json; charset=utf-8");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, r->body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(r->body));
    }
    h = curl_slist_append(h, "Expect:");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);

    r->http_code = 0;
    r->t_start = osGetTime(); r->t_first = 0; r->fb_timed_out = false;
    CURLcode rc = curl_easy_perform(c);
    r->curl_code = rc;
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    if (code) r->http_code = code;
    curl_slist_free_all(h);
    {   // registro de tiempos (sd:/3ds/Gemi3DS/net.log) para diagnosticar lentitud
        double tn = 0, tc = 0, ta = 0, ts = 0, tt = 0;
        curl_easy_getinfo(c, CURLINFO_NAMELOOKUP_TIME, &tn);
        curl_easy_getinfo(c, CURLINFO_CONNECT_TIME, &tc);
        curl_easy_getinfo(c, CURLINFO_APPCONNECT_TIME, &ta);
        curl_easy_getinfo(c, CURLINFO_STARTTRANSFER_TIME, &ts);
        curl_easy_getinfo(c, CURLINFO_TOTAL_TIME, &tt);
        FILE *lf = fopen("sdmc:/3ds/Gemi3DS/net.log", "a");
        if (lf) {
            const char *q = strchr(r->url, '?');
            fprintf(lf, "[%llu] %.*s http=%ld rc=%d dns=%.2f tcp=%.2f ssl=%.2f primer_byte=%.2f total=%.2f\n",
                    (unsigned long long)osGetTime(), q ? (int)(q - r->url) : (int)strlen(r->url), r->url,
                    r->http_code, rc, tn, tc, ta, ts, tt);
            if (r->http_code != 200 && r->resp && r->resp_len) fprintf(lf, "    respuesta: %.600s\n", r->resp);
            fclose(lf);
        }
    }
    if (rc != CURLE_OK) {
        if (r->fb_timed_out) snprintf(r->err, sizeof(r->err), _("El modelo tardó demasiado en responder"));
        else if (r->cancel) snprintf(r->err, sizeof(r->err), _("Cancelado"));
        else snprintf(r->err, sizeof(r->err), "%s", curl_easy_strerror(rc));
        r->state = REQ_FAILED;
    } else {
        r->state = REQ_DONE;
    }
}

static void worker(void *arg) {
    CURL *c = curl_easy_init();
    while (s_run) {
        LightEvent_Wait(&s_ev);
        while (s_run) {
            NetReq *r = NULL;
            LightLock_Lock(&s_qlock);
            if (s_qh != s_qt) { r = s_q[s_qh]; s_qh = (s_qh + 1) % QMAX; }
            LightLock_Unlock(&s_qlock);
            if (!r) break;
            if (r->cancel) { snprintf(r->err, sizeof(r->err), _("Cancelado")); r->state = REQ_FAILED; continue; }
            r->state = REQ_RUNNING;
            perform(c, r);
        }
    }
    curl_easy_cleanup(c);
}

bool net_init(void) {
    s_socbuf = memalign(SOC_ALIGN, SOC_SIZE);
    if (!s_socbuf) return false;
    if (R_FAILED(socInit(s_socbuf, SOC_SIZE))) { free(s_socbuf); s_socbuf = NULL; return false; }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    LightEvent_Init(&s_ev, RESET_ONESHOT);
    LightLock_Init(&s_qlock);
    s_run = true;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // en New 3DS usamos el núcleo 2; si no, el 1; si no, el mismo núcleo
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    s_thread = NULL;
    if (isNew) s_thread = threadCreate(worker, NULL, 256 * 1024, prio - 1, 2, false);
    if (!s_thread) {
        APT_SetAppCpuTimeLimit(30);
        s_thread = threadCreate(worker, NULL, 256 * 1024, prio - 1, 1, false);
    }
    if (!s_thread) s_thread = threadCreate(worker, NULL, 256 * 1024, prio + 1, -2, false);
    s_inited = s_thread != NULL;
    return s_inited;
}

void net_exit(void) {
    if (!s_inited) return;
    LightLock_Lock(&s_qlock);
    for (int i = s_qh; i != s_qt; i = (i + 1) % QMAX) s_q[i]->cancel = true;
    LightLock_Unlock(&s_qlock);
    s_run = false;
    LightEvent_Signal(&s_ev);
    if (R_SUCCEEDED(threadJoin(s_thread, 3000000000ULL))) threadFree(s_thread);
    curl_global_cleanup();
    socExit();
    free(s_socbuf);
    s_inited = false;
}

bool net_submit(NetReq *r) {
    if (!s_inited) { snprintf(r->err, sizeof(r->err), _("Red no disponible")); r->state = REQ_FAILED; return false; }
    LightLock_Lock(&s_qlock);
    int nt = (s_qt + 1) % QMAX;
    if (nt == s_qh) { LightLock_Unlock(&s_qlock); r->state = REQ_FAILED; snprintf(r->err, sizeof(r->err), _("Cola llena")); return false; }
    r->cancel = false;
    r->state = REQ_QUEUED;
    r->http_code = 0; r->err[0] = 0; r->resp_len = 0;
    if (r->resp) r->resp[0] = 0;
    s_q[s_qt] = r;
    s_qt = nt;
    LightLock_Unlock(&s_qlock);
    LightEvent_Signal(&s_ev);
    return true;
}

void net_req_reset(NetReq *r) {
    free(r->resp);
    r->resp = NULL; r->resp_len = r->resp_cap = 0;
    r->state = REQ_IDLE; r->cancel = false; r->err[0] = 0; r->http_code = 0;
}

bool net_busy(const NetReq *r) { return r->state == REQ_QUEUED || r->state == REQ_RUNNING; }

u32 net_local_ip(void) { return (u32)gethostid(); }

const char *net_strerror(NetReq *r) {
    switch (r->curl_code) {
    case CURLE_COULDNT_RESOLVE_HOST: return _("No hay conexión a Internet (DNS).");
    case CURLE_COULDNT_CONNECT: return _("No se pudo conectar con Google.");
    case CURLE_OPERATION_TIMEDOUT: return _("Se agotó el tiempo de espera.");
    case CURLE_PEER_FAILED_VERIFICATION: case CURLE_SSL_CACERT_BADFILE: return _("Error de certificado SSL.");
    case CURLE_SSL_CONNECT_ERROR: return _("Error en la conexión segura (SSL).");
    case CURLE_ABORTED_BY_CALLBACK: case CURLE_WRITE_ERROR: return r->cancel ? _("Detenido.") : _("Error de escritura.");
    default: return r->err[0] ? r->err : _("Error de red.");
    }
}

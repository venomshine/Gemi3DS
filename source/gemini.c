/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - cliente de la API de Gemini
//
// Política de modelos:
//  * Rápido / Razonamiento ("automáticos"): se usa el Flash más rápido disponible. Si un modelo
//    tarda demasiado en empezar a responder, está saturado (503) o sin cupo (429), se pasa al
//    siguiente y el modelo lento queda "en pausa" unos minutos para no volver a esperarlo.
//  * Pro o un modelo elegido a mano: se usa SIEMPRE ese modelo; si falla se explica el motivo.
#include "gemini.h"
#include "net.h"
#include "usage.h"
#include "store.h"
#include "util.h"
#include "i18n.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define API_DEFAULT "https://generativelanguage.googleapis.com/v1beta"
static const char *api(void) { return g_cfg.api_base[0] ? g_cfg.api_base : API_DEFAULT; }

#define SCPY(dst, src) snprintf(dst, sizeof(dst), "%s", src)

static NetReq s_chat, s_models, s_valid, s_title;
static Msg *s_target, *s_orphan;
static Chat *s_target_chat;
static bool s_chat_active, s_models_active, s_valid_active, s_title_active;
static char s_title_chat[24], s_title_model[80];
static char s_status[120];
static int s_mem_event;
static volatile int s_tok_in, s_tok_out;     // usageMetadata de la petición en curso (hilo de red)

// reintentos
static bool s_no_think, s_think_retry;
static char s_tried[6][80]; static int s_ntried;
static char s_first_model[80]; static int s_first_reason;   // 1 lento, 2 saturado, 3 sin cupo

// modelos en pausa (lentos o saturados)
typedef struct { char id[80]; u64 until; } Cool;
static Cool s_cool[8];

// buffer SSE (lo usa el hilo de red)
static char *s_sse; static size_t s_sse_len, s_sse_cap;

#define FAST_FIRST_BYTE_MS   14000
#define THINK_FIRST_BYTE_MS  40000
#define SLOW_MS              10000

// ------------------------------------------------------------ pausa de modelos
static u64 now_ms(void) { return osGetTime(); }

static bool cooled(const char *id) {
    for (int i = 0; i < 8; i++) if (s_cool[i].id[0] && !strcmp(s_cool[i].id, id) && now_ms() < s_cool[i].until) return true;
    return false;
}

static void cool(const char *id, int minutes) {
    int slot = 0;
    for (int i = 0; i < 8; i++) {
        if (!strcmp(s_cool[i].id, id)) { slot = i; break; }
        if (s_cool[i].until < s_cool[slot].until) slot = i;
    }
    SCPY(s_cool[slot].id, id);
    s_cool[slot].until = now_ms() + (u64)minutes * 60000ULL;
}

static bool model_available(const char *id) {
    if (!g_models.loaded || g_models.n == 0) return true;
    for (int i = 0; i < g_models.n; i++) if (!strcmp(g_models.list[i].id, id)) return true;
    return false;
}

// candidatos para los modos automáticos, del más rápido/fiable al menos
static int auto_candidates(const char **out, int max) {
    const char *c[] = {g_models.flash, "gemini-3.6-flash", "gemini-3.5-flash", "gemini-flash-latest",
                       "gemini-3.8-flash", "gemini-3-flash-preview", "gemini-2.5-flash",
                       g_models.lite, "gemini-3.5-flash-lite", "gemini-flash-lite-latest", NULL};
    int n = 0;
    for (int i = 0; c[i] && n < max; i++) {
        if (!c[i][0] || !model_available(c[i])) continue;
        bool dup = false;
        for (int k = 0; k < n; k++) if (!strcmp(out[k], c[i])) dup = true;
        if (!dup) out[n++] = c[i];
    }
    return n;
}

static bool tried(const char *id) {
    for (int k = 0; k < s_ntried; k++) if (!strcmp(s_tried[k], id)) return true;
    return false;
}

// siguiente modelo automático no probado (prefiere los que no están en pausa)
static const char *next_auto_model(void) {
    const char *c[12];
    int n = auto_candidates(c, 12);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++)
            if (!tried(c[i]) && (pass == 1 || !cooled(c[i]))) return c[i];
    return NULL;
}

// ------------------------------------------------------------ configuración
void gem_api_init(void) {
    if (!g_models.flash[0]) SCPY(g_models.flash, "gemini-3.6-flash");
    if (!g_models.lite[0]) SCPY(g_models.lite, "gemini-3.5-flash-lite");
}

bool gem_mode_auto(void) { return g_cfg.mode == MODE_FAST || g_cfg.mode == MODE_THINK; }

const char *gem_current_model(void) {
    switch (g_cfg.mode) {
    case MODE_PRO: return g_models.pro[0] ? g_models.pro : g_models.flash;
    case MODE_CUSTOM: return g_cfg.custom_model[0] ? g_cfg.custom_model : g_models.flash;
    default: {
        const char *c[12];
        int n = auto_candidates(c, 12);
        for (int i = 0; i < n; i++) if (!cooled(c[i])) return c[i];
        return g_models.flash;
    }
    }
}

const char *gem_mode_label(int mode) {
    switch (mode) {
    case MODE_FAST: return _("Rápido");
    case MODE_THINK: return _("Razonamiento");
    case MODE_PRO: return "Pro";
    default: {
        static char buf[64];
        const char *m = g_cfg.custom_model;
        for (int i = 0; i < g_models.n; i++)
            if (!strcmp(g_models.list[i].id, m)) { SCPY(buf, g_models.list[i].name); return buf; }
        SCPY(buf, m);
        return buf;
    }
    }
}

const char *gem_status(void) { return s_chat_active ? s_status : ""; }

int gem_model_paused(const char *id) {
    for (int i = 0; i < 8; i++)
        if (s_cool[i].id[0] && !strcmp(s_cool[i].id, id) && now_ms() < s_cool[i].until)
            return (int)((s_cool[i].until - now_ms()) / 1000) + 1;
    return 0;
}

const char *gem_active_model(void) { return s_chat_active && s_target ? s_target->model : ""; }

int gem_take_memory_event(void) { int e = s_mem_event; s_mem_event = 0; return e; }

// ------------------------------------------------------------ errores
static void api_message(NetReq *r, char *msg, int n) {
    msg[0] = 0;
    if (!r->resp || !r->resp_len) return;
    cJSON *j = cJSON_Parse(r->resp);
    cJSON *e = j ? cJSON_GetObjectItem(j, "error") : NULL;
    if (!e && cJSON_IsArray(j)) e = cJSON_GetObjectItem(cJSON_GetArrayItem(j, 0), "error");
    cJSON *m = e ? cJSON_GetObjectItem(e, "message") : NULL;
    if (cJSON_IsString(m)) snprintf(msg, n, "%s", m->valuestring);
    cJSON_Delete(j);
}

static void api_error_text(NetReq *r, const char *model, bool manual, char *out, int n) {
    char msg[400];
    api_message(r, msg, sizeof(msg));
    long c = r->http_code;
    if (r->fb_timed_out)
        snprintf(out, n, _("Gemini tardó demasiado en empezar a responder. Inténtalo de nuevo en un momento."));
    else if (c == 400 && (strstr(msg, "API key") || strstr(msg, "API_KEY")))
        snprintf(out, n, _("Tu clave de API no es válida. Vuelve a vincular tu cuenta en Ajustes."));
    else if (c == 400) snprintf(out, n, _("Solicitud no válida.\n\n%s"), msg);
    else if (c == 401 || c == 403) snprintf(out, n, _("Tu clave no tiene permiso para usar **%s**.\n\n%s"), model ? model : "", msg);
    else if (c == 404) snprintf(out, n, _("El modelo **%s** no existe o no está disponible con tu clave. Elige otro en el selector de modelo."), model ? model : "");
    else if (c == 429) {
        char wait[64] = "";
        const char *rd = r->resp ? strstr(r->resp, "\"retryDelay\"") : NULL;
        int secs = 0;
        if (rd) { const char *q = strchr(rd + 12, '"'); if (q) sscanf(q + 1, "%d", &secs); }
        if (secs > 0) snprintf(wait, sizeof(wait), _(" Vuelve a intentarlo en %d s."), secs);
        bool zero = r->resp && (strstr(r->resp, "limit: 0") || strstr(r->resp, "\"limit\": 0"));
        bool daily = r->resp && strstr(r->resp, "PerDay");
        if (manual && (zero || strstr(msg, "billing")))
            snprintf(out, n, _("**%s** no está incluido en el nivel gratuito de tu clave. Para usarlo hay que activar la facturación "
                             "en Google AI Studio (aistudio.google.com → Facturación). Si ya pagas, revisa los límites de tu proyecto. "
                             "Mientras tanto, usa el modo **Rápido**."), model ? model : _("Este modelo"));
        else if (daily)
            snprintf(out, n, _("Se agotó el cupo gratuito de hoy para **%s**. Cambia de modelo o vuelve mañana."), model ? model : _("este modelo"));
        else
            snprintf(out, n, _("Google rechazó la petición por límite de uso (429) en **%s**.%s"), model ? model : _("este modelo"), wait);
    }
    else if (c >= 500) snprintf(out, n, _("Gemini (%s) está saturado ahora mismo (%ld). Inténtalo de nuevo en un momento%s."),
                                model ? model : "", c, manual ? _(" o usa el modo **Rápido**") : "");
    else if (c) snprintf(out, n, "Error %ld: %s", c, msg);
    else snprintf(out, n, "%s", net_strerror(r));
}

// ------------------------------------------------------------ modelos
static bool has(const char *s, const char *sub) { return strstr(s, sub) != NULL; }
static bool excluded(const char *id) {
    static const char *bad[] = {"tts", "image", "live", "embedding", "transcribe", "robotics", "audio", "computer",
                                "veo", "lyria", "deep-research", "antigravity", "aqa", "omni", "translate", "banana", NULL};
    for (int i = 0; bad[i]; i++) if (has(id, bad[i])) return true;
    return false;
}

static void pick(char *dst, size_t dn, char ids[][80], int n, const char **prefs, const char *must, const char *mustnot) {
    for (int p = 0; prefs[p]; p++)
        for (int i = 0; i < n; i++)
            if (!strcmp(ids[i], prefs[p])) { snprintf(dst, dn, "%s", ids[i]); return; }
    for (int i = 0; i < n; i++)
        if (has(ids[i], must) && (!mustnot || !has(ids[i], mustnot)) && !strncmp(ids[i], "gemini", 6)) {
            snprintf(dst, dn, "%s", ids[i]); return;
        }
}

void gem_models_fetch(void) {
    if (s_models_active || !g_cfg.api_key[0]) return;
    net_req_reset(&s_models);
    snprintf(s_models.url, sizeof(s_models.url), "%s/models?pageSize=200", api());
    SCPY(s_models.api_key, g_cfg.api_key);
    s_models.body = NULL; s_models.on_data = NULL; s_models.first_byte_timeout_ms = 0;
    s_models_active = net_submit(&s_models);
}

bool gem_models_busy(void) { return s_models_active; }

void gem_models_poll(void) {
    if (!s_models_active || net_busy(&s_models)) return;
    s_models_active = false;
    if (s_models.state != REQ_DONE || s_models.http_code != 200 || !s_models.resp) return;
    cJSON *j = cJSON_Parse(s_models.resp);
    if (!j) return;
    static char txt[200][80];
    int nt = 0;
    g_models.n = 0;
    cJSON *arr = cJSON_GetObjectItem(j, "models"), *m;
    cJSON_ArrayForEach(m, arr) {
        cJSON *nm = cJSON_GetObjectItem(m, "name");
        if (!cJSON_IsString(nm)) continue;
        const char *id = nm->valuestring;
        if (!strncmp(id, "models/", 7)) id += 7;
        bool gen = false;
        cJSON *meths = cJSON_GetObjectItem(m, "supportedGenerationMethods"), *x;
        cJSON_ArrayForEach(x, meths) if (cJSON_IsString(x) && !strcmp(x->valuestring, "generateContent")) gen = true;
        if (!gen || excluded(id) || (strncmp(id, "gemini", 6) && strncmp(id, "gemma", 5))) continue;
        if (nt < 200) SCPY(txt[nt++], id);
        if (g_models.n >= 48) continue;
        ModelInfo *mi = &g_models.list[g_models.n++];
        SCPY(mi->id, id);
        cJSON *dn = cJSON_GetObjectItem(m, "displayName");
        cJSON *de = cJSON_GetObjectItem(m, "description");
        SCPY(mi->name, cJSON_IsString(dn) ? dn->valuestring : id);
        SCPY(mi->desc, cJSON_IsString(de) ? de->valuestring : "");
    }
    cJSON_Delete(j);
    if (nt == 0) return;
    // el primero de la lista es el preferido para _("Rápido"): rápido y con buen cupo gratuito
    static const char *pf[] = {"gemini-3.6-flash", "gemini-3.5-flash", "gemini-flash-latest", "gemini-3.8-flash", "gemini-3-flash-preview", "gemini-2.5-flash", NULL};
    static const char *pp[] = {"gemini-3.5-pro", "gemini-pro-latest", "gemini-3.1-pro-preview", "gemini-3-pro", "gemini-2.5-pro", NULL};
    static const char *pl[] = {"gemini-3.5-flash-lite", "gemini-flash-lite-latest", "gemini-3.1-flash-lite", "gemini-2.5-flash-lite", NULL};
    char f[80] = "", p[80] = "", l[80] = "";
    pick(f, sizeof(f), txt, nt, pf, "flash", "lite");
    pick(p, sizeof(p), txt, nt, pp, "pro", NULL);
    pick(l, sizeof(l), txt, nt, pl, "lite", NULL);
    if (f[0]) SCPY(g_models.flash, f);
    SCPY(g_models.pro, p);
    SCPY(g_models.lite, l[0] ? l : g_models.flash);
    g_models.loaded = true;
    config_save();
}

// ------------------------------------------------------------ validación
void gem_validate_key(const char *key) {
    if (s_valid_active) return;
    net_req_reset(&s_valid);
    snprintf(s_valid.url, sizeof(s_valid.url), "%s/models?pageSize=200", api());
    SCPY(s_valid.api_key, key);
    s_valid.first_byte_timeout_ms = 0;
    s_valid_active = net_submit(&s_valid);
}

int gem_validate_poll(char *msg, int n) {
    if (!s_valid_active) return 0;
    if (net_busy(&s_valid)) return -1;
    s_valid_active = false;
    if (s_valid.state == REQ_DONE && s_valid.http_code == 200) {
        if (!s_models_active) {   // reutilizar la respuesta como lista de modelos
            net_req_reset(&s_models);
            s_models.resp = s_valid.resp; s_models.resp_len = s_valid.resp_len; s_models.resp_cap = s_valid.resp_cap;
            s_valid.resp = NULL; s_valid.resp_len = s_valid.resp_cap = 0;
            s_models.state = REQ_DONE; s_models.http_code = 200;
            s_models_active = true;
            gem_models_poll();
        }
        snprintf(msg, n, _("Conectado"));
        return 1;
    }
    api_error_text(&s_valid, NULL, false, msg, n);
    return 0;
}

// ------------------------------------------------------------ chat
static void sys_instruction(char *out, int n) {
    time_t now = time(NULL);
    struct tm tmv; gmtime_r(&now, &tmv);
    static const char *M[] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre"};
    int o = snprintf(out, n,
        "Eres Gemini, un asistente de IA de Google. El usuario te habla desde Gemi3DS, un cliente para Nintendo 3DS "
        "con una pantalla pequeña (400x240). Da respuestas claras, útiles y bien estructuradas, y evita respuestas "
        "excesivamente largas salvo que se pida detalle. Usa Markdown sencillo (negritas, títulos, listas y bloques de código); "
        "evita tablas muy anchas, LaTeX y emojis, porque la consola no los muestra bien. Responde en el idioma del usuario. "
        "Fecha actual: %d de %s de %d.\n\n"
        "En esta app NO puedes generar ni editar imágenes. Si el usuario te pide crear una imagen, explícale amablemente que "
        "la 3DS no puede hacerlo (generar y mostrar imágenes requiere mucha más memoria, potencia y resolución de pantalla de "
        "las que tiene la consola) y recomiéndale usar Gemini en el navegador (gemini.google.com) o en la app para Android o iPhone.\n\n"
        "Memoria: si el usuario te pide EXPLÍCITAMENTE que recuerdes algo sobre él (por ejemplo «recuerda que…» o «guarda que…»), "
        "confírmalo y añade al final de tu respuesta una línea con el formato exacto [[RECORDAR: dato breve]]. Si te pide "
        "olvidar algo, añade [[OLVIDAR: palabras clave]]. No uses esas etiquetas en ningún otro caso.",
        tmv.tm_mday, M[tmv.tm_mon % 12], tmv.tm_year + 1900);
    if (g_lang != LANG_ES && o < n)
        o += snprintf(out + o, n - o, "\n\nLa interfaz de la app está en %s: si el usuario no ha escrito todavía en otro idioma, "
                      "responde en %s.", g_lang == LANG_JA ? "japonés" : "inglés", g_lang == LANG_JA ? "japonés" : "inglés");
    if (g_cfg.name[0] && o < n) o += snprintf(out + o, n - o, "\n\nEl usuario se llama %s.", g_cfg.name);
    if (g_cfg.saved_info[0] && o < n) o += snprintf(out + o, n - o, "\n\nInformación que el usuario te pidió recordar (úsala cuando sea útil):\n%s", g_cfg.saved_info);
}

static void add_attachment(cJSON *parts, Msg *m) {
    if (!m->att || !m->att_path[0]) return;
    size_t sz = 0;
    u8 *d = file_read(m->att_path, &sz);
    if (!d) return;
    if (sz > 0 && sz < 6 * 1024 * 1024) {
        char *b = b64_encode(d, sz, NULL);
        if (b) {
            cJSON *p = cJSON_CreateObject();
            cJSON *id = cJSON_AddObjectToObject(p, "inlineData");
            cJSON_AddStringToObject(id, "mimeType", m->att_mime[0] ? m->att_mime : "image/jpeg");
            cJSON_AddStringToObject(id, "data", b);
            cJSON_AddItemToArray(parts, p);
            free(b);
        }
    }
    free(d);
}

static bool is_gemini2(const char *m) { return !strncmp(m, "gemini-2", 8); }

static char *build_body(Chat *c, Msg *target, const char *model) {
    cJSON *root = cJSON_CreateObject();
    static char sys[6000];
    sys_instruction(sys, sizeof(sys));
    cJSON *si = cJSON_AddObjectToObject(root, "systemInstruction");
    cJSON *sp = cJSON_AddArrayToObject(si, "parts");
    cJSON *spp = cJSON_CreateObject();
    cJSON_AddStringToObject(spp, "text", sys);
    cJSON_AddItemToArray(sp, spp);

    cJSON *contents = cJSON_AddArrayToObject(root, "contents");
    int last = -1;
    for (int i = 0; i < c->n; i++) if (c->m[i] == target) { last = i; break; }
    if (last < 0) last = c->n;
    for (int i = 0; i < last; i++) {
        Msg *m = c->m[i];
        if (m->role == ROLE_USER) {
            // si la respuesta siguiente falló, omitimos el par
            if (i + 1 < last && c->m[i + 1]->role == ROLE_MODEL &&
                (c->m[i + 1]->state == MST_ERROR || c->m[i + 1]->tlen == 0)) { i++; continue; }
            cJSON *ct = cJSON_CreateObject();
            cJSON_AddStringToObject(ct, "role", "user");
            cJSON *parts = cJSON_AddArrayToObject(ct, "parts");
            if (i >= last - 5) add_attachment(parts, m);
            const char *t = m->tlen ? m->text : (m->att == ATT_AUDIO ? "(Nota de voz del usuario: escúchala y responde a lo que dice, en su idioma.)" : "Describe esta imagen.");
            cJSON *tp = cJSON_CreateObject();
            cJSON_AddStringToObject(tp, "text", t);
            cJSON_AddItemToArray(parts, tp);
            cJSON_AddItemToArray(contents, ct);
        } else {
            if (m->state == MST_ERROR || m->tlen == 0) continue;
            cJSON *ct = cJSON_CreateObject();
            cJSON_AddStringToObject(ct, "role", "model");
            cJSON *parts = cJSON_AddArrayToObject(ct, "parts");
            cJSON *tp = cJSON_CreateObject();
            cJSON_AddStringToObject(tp, "text", m->text);
            if (m->sig && !strcmp(m->model, model)) cJSON_AddStringToObject(tp, "thoughtSignature", m->sig);
            cJSON_AddItemToArray(parts, tp);
            cJSON_AddItemToArray(contents, ct);
        }
    }
    if (!strncmp(model, "gemini", 6) && !s_no_think) {
        cJSON *gc = cJSON_AddObjectToObject(root, "generationConfig");
        cJSON *tc = cJSON_AddObjectToObject(gc, "thinkingConfig");
        bool think = g_cfg.mode != MODE_FAST;
        if (g_cfg.mode == MODE_CUSTOM) {
            // modelo elegido a mano: se respeta su razonamiento por defecto
        } else if (is_gemini2(model)) {
            if (!think && strstr(model, "flash")) cJSON_AddNumberToObject(tc, "thinkingBudget", 0);
            else cJSON_AddNumberToObject(tc, "thinkingBudget", -1);
        } else {
            cJSON_AddStringToObject(tc, "thinkingLevel", think ? "high" : "low");
        }
        cJSON_AddBoolToObject(tc, "includeThoughts", think && g_cfg.show_thoughts);
    }
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

static void handle_event(const char *json) {
    cJSON *j = cJSON_Parse(json);
    if (!j) return;
    Msg *m = s_target;
    cJSON *err = cJSON_GetObjectItem(j, "error");
    if (err) {
        cJSON *em = cJSON_GetObjectItem(err, "message");
        LightLock_Lock(&g_chat_lock);
        { const char *eh = _("\n\n**Error:** "); msg_append(m, eh, strlen(eh)); }
        if (cJSON_IsString(em)) msg_append(m, em->valuestring, strlen(em->valuestring));
        m->state = MST_ERROR;
        LightLock_Unlock(&g_chat_lock);
        cJSON_Delete(j);
        return;
    }
    cJSON *um = cJSON_GetObjectItem(j, "usageMetadata");
    if (um) {
        cJSON *pi = cJSON_GetObjectItem(um, "promptTokenCount"), *co = cJSON_GetObjectItem(um, "candidatesTokenCount"),
              *tt = cJSON_GetObjectItem(um, "thoughtsTokenCount");
        if (cJSON_IsNumber(pi)) s_tok_in = pi->valueint;
        s_tok_out = (cJSON_IsNumber(co) ? co->valueint : 0) + (cJSON_IsNumber(tt) ? tt->valueint : 0);
    }
    cJSON *cand = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "candidates"), 0);
    cJSON *pf = cJSON_GetObjectItem(j, "promptFeedback");
    cJSON *br = pf ? cJSON_GetObjectItem(pf, "blockReason") : NULL;
    if (cJSON_IsString(br)) {
        const char *t = _("Lo siento, no puedo responder a eso (contenido bloqueado por seguridad).");
        LightLock_Lock(&g_chat_lock); msg_append(m, t, strlen(t)); LightLock_Unlock(&g_chat_lock);
    }
    if (cand) {
        cJSON *parts = cJSON_GetObjectItem(cJSON_GetObjectItem(cand, "content"), "parts"), *p;
        cJSON_ArrayForEach(p, parts) {
            cJSON *txt = cJSON_GetObjectItem(p, "text");
            cJSON *th = cJSON_GetObjectItem(p, "thought");
            cJSON *sig = cJSON_GetObjectItem(p, "thoughtSignature");
            LightLock_Lock(&g_chat_lock);
            if (cJSON_IsString(sig)) { free(m->sig); m->sig = strdup(sig->valuestring); }
            if (cJSON_IsString(txt)) {
                if (cJSON_IsTrue(th)) msg_append_thought(m, txt->valuestring, strlen(txt->valuestring));
                else msg_append(m, txt->valuestring, strlen(txt->valuestring));
            }
            LightLock_Unlock(&g_chat_lock);
        }
        cJSON *fr = cJSON_GetObjectItem(cand, "finishReason");
        if (cJSON_IsString(fr)) {
            const char *r = fr->valuestring, *note = NULL;
            if (!strcmp(r, "SAFETY") || !strcmp(r, "PROHIBITED_CONTENT") || !strcmp(r, "BLOCKLIST") || !strcmp(r, "SPII"))
                note = _("\n\n*(Respuesta detenida por los filtros de seguridad.)*");
            else if (!strcmp(r, "RECITATION")) note = _("\n\n*(Respuesta detenida por posible contenido con derechos de autor.)*");
            else if (!strcmp(r, "MAX_TOKENS")) note = _("\n\n*(Respuesta cortada por longitud.)*");
            if (note) { LightLock_Lock(&g_chat_lock); msg_append(m, note, strlen(note)); LightLock_Unlock(&g_chat_lock); }
        }
    }
    cJSON_Delete(j);
}

static void sse_cb(NetReq *r, const char *data, size_t len) {
    if (s_sse_len + len + 1 > s_sse_cap) {
        size_t nc = s_sse_cap ? s_sse_cap * 2 : 65536;
        while (nc < s_sse_len + len + 1) nc *= 2;
        char *nb = realloc(s_sse, nc);
        if (!nb) return;
        s_sse = nb; s_sse_cap = nc;
    }
    memcpy(s_sse + s_sse_len, data, len);
    s_sse_len += len;
    s_sse[s_sse_len] = 0;
    for (;;) {
        char *e1 = strstr(s_sse, "\n\n");
        char *e2 = strstr(s_sse, "\r\n\r\n");
        char *end = e1; int sep = 2;
        if (e2 && (!e1 || e2 < e1)) { end = e2; sep = 4; }
        if (!end) break;
        *end = 0;
        char *line = s_sse;
        static char *acc; static size_t acc_cap;
        size_t acc_len = 0;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            size_t ll = strlen(line);
            if (ll && line[ll - 1] == '\r') line[--ll] = 0;
            if (!strncmp(line, "data:", 5)) {
                char *d = line + 5;
                if (*d == ' ') d++;
                size_t dl = strlen(d);
                if (acc_len + dl + 2 > acc_cap) { acc_cap = (acc_len + dl + 2) * 2; acc = realloc(acc, acc_cap); }
                memcpy(acc + acc_len, d, dl); acc_len += dl; acc[acc_len] = 0;
            }
            line = nl ? nl + 1 : NULL;
        }
        if (acc_len) handle_event(acc);
        size_t used = (end - s_sse) + sep;
        memmove(s_sse, s_sse + used, s_sse_len - used + 1);
        s_sse_len -= used;
    }
}

static bool submit_with(Chat *c, Msg *target, const char *model) {
    SCPY(target->model, model);
    net_req_reset(&s_chat);
    free(s_chat.body);
    s_chat.body = build_body(c, target, model);
    snprintf(s_chat.url, sizeof(s_chat.url), "%s/models/%s:streamGenerateContent?alt=sse", api(), model);
    SCPY(s_chat.api_key, g_cfg.api_key);
    s_chat.on_data = sse_cb;
    s_chat.first_byte_timeout_ms = g_cfg.mode == MODE_FAST ? FAST_FIRST_BYTE_MS : (g_cfg.mode == MODE_THINK ? THINK_FIRST_BYTE_MS : 0);
    s_sse_len = 0;
    if (s_sse) s_sse[0] = 0;
    s_tok_in = s_tok_out = 0;
    s_target = target;
    s_target_chat = c;
    target->state = MST_STREAM;
    if (s_ntried < 6) SCPY(s_tried[s_ntried++], model);
    s_chat_active = net_submit(&s_chat);
    if (!s_chat_active) { target->state = MST_ERROR; msg_set_text(target, _("No se pudo iniciar la solicitud.")); }
    return s_chat_active;
}

bool gem_send(Chat *c, Msg *target) {
    if (s_chat_active) return false;
    s_no_think = false; s_think_retry = false;
    s_ntried = 0; s_first_model[0] = 0; s_first_reason = 0;
    s_status[0] = 0;
    return submit_with(c, target, gem_current_model());
}

void gem_stop(void) { if (s_chat_active) s_chat.cancel = true; }

void gem_stop_wait(void) {
    gem_stop();
    for (int i = 0; i < 300 && s_chat_active && net_busy(&s_chat); i++) svcSleepThread(10000000ULL);
    if (s_chat_active && net_busy(&s_chat)) {
        // el hilo de red sigue ocupado (p. ej. resolviendo DNS): redirigir su salida
        if (!s_orphan) s_orphan = msg_new(ROLE_MODEL);
        LightLock_Lock(&g_chat_lock);
        if (s_target) s_target->state = MST_STOPPED;
        s_target = s_orphan;
        s_target_chat = NULL;
        LightLock_Unlock(&g_chat_lock);
        return;
    }
    gem_poll();
}

bool gem_streaming(void) { return s_chat_active; }
Msg *gem_target(void) { return s_chat_active ? s_target : NULL; }

// procesa etiquetas [[RECORDAR: …]] / [[OLVIDAR: …]] y las quita del texto
static void process_memory_tags(Msg *m) {
    if (!m->text || !strstr(m->text, "[[")) return;
    char *out = malloc(m->tlen + 1);
    int o = 0;
    const char *p = m->text;
    while (*p) {
        bool rem = !strncasecmp(p, "[[RECORDAR:", 11), forg = !strncasecmp(p, "[[OLVIDAR:", 10);
        const char *e = (rem || forg) ? strstr(p, "]]") : NULL;
        if (e) {
            char item[400];
            const char *s = p + (rem ? 11 : 10);
            int l = (int)(e - s);
            if (l > 399) l = 399;
            memcpy(item, s, l); item[l] = 0;
            str_trim(item);
            if (item[0]) {
                if (rem) { mem_add(item); s_mem_event++; }
                else { int k = mem_forget(item); if (k) s_mem_event -= k; }
            }
            p = e + 2;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    str_trim(out);
    msg_set_text(m, out);
    free(out);
}

int gem_poll(void) {
    if (!s_chat_active || net_busy(&s_chat)) return 0;
    s_chat_active = false;
    Msg *m = s_target;
    if (m == s_orphan) {
        LightLock_Lock(&g_chat_lock);
        msg_set_text(m, "");
        m->thlen = 0;
        LightLock_Unlock(&g_chat_lock);
        free(s_chat.body); s_chat.body = NULL;
        s_target = NULL;
        return 0;
    }
    if (s_sse_len) sse_cb(&s_chat, "\n\n", 2);
    LightLock_Lock(&g_chat_lock);
    bool empty = m->tlen == 0;
    bool user_cancel = s_chat.cancel && !s_chat.fb_timed_out;
    long code = s_chat.http_code;
    if (code)                 // cada intento que llegó a Google cuenta para el cupo
        usage_record(m->model, code, s_tok_in, s_tok_out,
                     s_chat.t_first ? (int)(s_chat.t_first - s_chat.t_start) : 0, s_chat.resp);
    bool retryable = s_chat.fb_timed_out || code == 429 || code == 500 || code == 503 || code == 504 ||
                     (code == 404 && s_ntried > 1);
    const char *next = NULL;

    if (user_cancel) {
        m->state = MST_STOPPED;
        if (empty) msg_set_text(m, _("*Respuesta detenida.*"));
    } else if (code == 400 && !s_think_retry && s_target_chat && empty && s_chat.resp &&
               (strstr(s_chat.resp, "hinking") || strstr(s_chat.resp, "thought"))) {
        // el modelo no admite la configuración de razonamiento: reintentar sin ella
        LightLock_Unlock(&g_chat_lock);
        s_think_retry = true; s_no_think = true;
        s_ntried--;
        submit_with(s_target_chat, m, m->model);
        return 0;
    } else if (empty && retryable && gem_mode_auto() && s_target_chat && (next = next_auto_model())) {
        // modo automático: este modelo está lento/saturado/sin cupo -> pausarlo y probar otro
        int reason = s_chat.fb_timed_out ? 1 : (code == 429 ? 3 : 2);
        cool(m->model, reason == 3 ? 5 : (reason == 1 ? 10 : 3));
        if (!s_first_model[0]) { SCPY(s_first_model, m->model); s_first_reason = reason; }
        msg_set_text(m, "");
        m->thlen = 0;
        snprintf(s_status, sizeof(s_status), _("%s %s, probando con %s…"), m->model,
                 reason == 1 ? _("va lento") : (reason == 3 ? _("no tiene cupo") : _("está saturado")), next);
        LightLock_Unlock(&g_chat_lock);
        free(s_chat.body); s_chat.body = NULL;
        submit_with(s_target_chat, m, next);
        return 0;
    } else if (s_chat.state == REQ_FAILED || code != 200) {
        char e[900];
        api_error_text(&s_chat, m->model, !gem_mode_auto(), e, sizeof(e));
        if (!empty) { msg_append(m, "\n\n", 2); msg_append(m, e, strlen(e)); }
        else msg_set_text(m, e);
        if (s_ntried > 1 && gem_mode_auto()) {
            const char *x = _("\n\n*(También se probó con otros modelos gratuitos sin éxito.)*");
            msg_append(m, x, strlen(x));
        }
        m->state = MST_ERROR;
    } else {
        if (m->state == MST_STREAM) m->state = MST_DONE;
        if (empty) {
            msg_set_text(m, _("No se recibió respuesta. Inténtalo de nuevo."));
            m->state = MST_ERROR;
        } else {
            process_memory_tags(m);
            // si tardó mucho en empezar, pausar el modelo para los próximos mensajes
            if (gem_mode_auto() && s_chat.t_first && s_chat.t_first - s_chat.t_start > SLOW_MS) cool(m->model, 10);
        }
    }
    m->ver++;
    LightLock_Unlock(&g_chat_lock);
    free(s_chat.body); s_chat.body = NULL;
    s_target = NULL;
    s_status[0] = 0;
    return 1;
}

// ------------------------------------------------------------ títulos
void gem_title_request(Chat *c) {
    if (s_title_active || c->temporary || c->n < 2) return;
    Msg *u = NULL, *a = NULL;
    for (int i = 0; i < c->n; i++) {
        if (!u && c->m[i]->role == ROLE_USER) u = c->m[i];
        else if (u && !a && c->m[i]->role == ROLE_MODEL && c->m[i]->state == MST_DONE) a = c->m[i];
    }
    if (!u || !a) return;
    char ut[420], at[420];
    snprintf(ut, sizeof(ut), "%.400s", u->tlen ? u->text : "(foto o nota de voz)");
    snprintf(at, sizeof(at), "%.400s", a->text);
    char prompt[1100];
    snprintf(prompt, sizeof(prompt),
        "Escribe un título muy corto (máximo 5 palabras) para esta conversación, en el mismo idioma del usuario. "
        "Responde SOLO con el título, sin comillas ni puntuación final.\n\nUsuario: %s\n\nAsistente: %s", ut, at);
    cJSON *root = cJSON_CreateObject();
    cJSON *cts = cJSON_AddArrayToObject(root, "contents");
    cJSON *ct = cJSON_CreateObject();
    cJSON_AddStringToObject(ct, "role", "user");
    cJSON *parts = cJSON_AddArrayToObject(ct, "parts");
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "text", prompt);
    cJSON_AddItemToArray(parts, p);
    cJSON_AddItemToArray(cts, ct);
    const char *model = g_models.lite[0] ? g_models.lite : g_models.flash;
    if (!strncmp(model, "gemini", 6) && !is_gemini2(model)) {
        cJSON *gc = cJSON_AddObjectToObject(root, "generationConfig");
        cJSON *tc = cJSON_AddObjectToObject(gc, "thinkingConfig");
        cJSON_AddStringToObject(tc, "thinkingLevel", "low");
    }
    net_req_reset(&s_title);
    free(s_title.body);
    s_title.body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    snprintf(s_title.url, sizeof(s_title.url), "%s/models/%s:generateContent", api(), model);
    SCPY(s_title.api_key, g_cfg.api_key);
    s_title.on_data = NULL;
    s_title.first_byte_timeout_ms = 20000;
    SCPY(s_title_chat, c->id);
    SCPY(s_title_model, model);
    s_title_active = net_submit(&s_title);
}

void gem_title_poll(void) {
    if (!s_title_active || net_busy(&s_title)) return;
    s_title_active = false;
    free(s_title.body); s_title.body = NULL;
    if (s_title.http_code) {
        int tin = 0, tout = 0;
        cJSON *uj = s_title.resp ? cJSON_Parse(s_title.resp) : NULL;
        cJSON *um = uj ? cJSON_GetObjectItem(uj, "usageMetadata") : NULL;
        if (um) {
            cJSON *pi = cJSON_GetObjectItem(um, "promptTokenCount"), *co = cJSON_GetObjectItem(um, "candidatesTokenCount"),
                  *tt = cJSON_GetObjectItem(um, "thoughtsTokenCount");
            tin = cJSON_IsNumber(pi) ? pi->valueint : 0;
            tout = (cJSON_IsNumber(co) ? co->valueint : 0) + (cJSON_IsNumber(tt) ? tt->valueint : 0);
        }
        cJSON_Delete(uj);
        usage_record(s_title_model, s_title.http_code, tin, tout, 0, s_title.resp);
    }
    if (s_title.state != REQ_DONE || s_title.http_code != 200 || !s_title.resp) return;
    cJSON *j = cJSON_Parse(s_title.resp);
    if (!j) return;
    cJSON *parts = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "candidates"), 0), "content"), "parts"), *p;
    char title[100] = "";
    cJSON_ArrayForEach(p, parts) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(p, "thought"))) continue;
        cJSON *t = cJSON_GetObjectItem(p, "text");
        if (cJSON_IsString(t)) strncat(title, t->valuestring, sizeof(title) - strlen(title) - 1);
    }
    cJSON_Delete(j);
    char *s = title;
    for (char *q = s; *q; q++) if (*q == '\n' || *q == '"' || *q == '*' || *q == '#') *q = ' ';
    str_trim(s);
    int l = strlen(s);
    if (l > 0 && s[l - 1] == '.') s[l - 1] = 0;
    if (!s[0]) return;
    if (g_chat && !strcmp(g_chat->id, s_title_chat)) {
        SCPY(g_chat->title, s);
        g_chat->titled = true;
        chat_save(g_chat);
    } else {
        Chat *c = chat_load(s_title_chat);
        if (c) { SCPY(c->title, s); c->titled = true; chat_save(c); chat_free(c); }
    }
}

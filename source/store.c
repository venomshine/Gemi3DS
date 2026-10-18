/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - persistencia en la SD (configuración, chats, gems)
#include "store.h"
#include "util.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>

Config g_cfg;
Models g_models;
Chat *g_chat;
LightLock g_chat_lock;
ChatMeta g_index[300];
int g_nindex;

// ------------------------------------------------------------ mensajes
Msg *msg_new(int role) {
    Msg *m = calloc(1, sizeof(Msg));
    m->role = role;
    m->text = calloc(1, 64); m->tcap = 64;
    m->time = osGetTime();
    m->appear = 0;
    return m;
}

void msg_free(Msg *m) {
    if (!m) return;
    free(m->text); free(m->thought); free(m->sig);
    md_free(&m->lay); md_free(&m->tlay);
    tex_free(&m->thumb);
    free(m);
}

static void buf_append(char **b, int *len, int *cap, const char *s, int n) {
    if (*len + n + 1 > *cap) {
        int nc = *cap ? *cap : 64;
        while (nc < *len + n + 1) nc *= 2;
        char *nb = realloc(*b, nc);
        if (!nb) return;
        *b = nb; *cap = nc;
    }
    memcpy(*b + *len, s, n);
    *len += n;
    (*b)[*len] = 0;
}

void msg_append(Msg *m, const char *s, int n) { buf_append(&m->text, &m->tlen, &m->tcap, s, n); m->ver++; }
void msg_append_thought(Msg *m, const char *s, int n) { buf_append(&m->thought, &m->thlen, &m->thcap, s, n); m->ver++; }
void msg_set_text(Msg *m, const char *s) { m->tlen = 0; if (m->text) m->text[0] = 0; msg_append(m, s, strlen(s)); }

Chat *chat_new(void) {
    Chat *c = calloc(1, sizeof(Chat));
    make_id(c->id, sizeof(c->id));
    c->updated = osGetTime();
    return c;
}

void chat_free(Chat *c) {
    if (!c) return;
    for (int i = 0; i < c->n; i++) msg_free(c->m[i]);
    free(c->m);
    free(c);
}

void chat_push(Chat *c, Msg *m) {
    if (c->n >= c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->m = realloc(c->m, c->cap * sizeof(Msg *));
    }
    c->m[c->n++] = m;
}

void chat_truncate(Chat *c, int n) {
    while (c->n > n) msg_free(c->m[--c->n]);
}

// ------------------------------------------------------------ config
static const char *jstr(cJSON *o, const char *k, const char *def) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}
static double jnum(cJSON *o, const char *k, double def) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}
static bool jbool(cJSON *o, const char *k, bool def) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : def;
}
#define SCPY(dst, src) snprintf(dst, sizeof(dst), "%s", src)

void store_init(void) {
    mkdirs(APP_DIR);
    mkdirs(CHATS_DIR);
    mkdirs(MEDIA_DIR);
    LightLock_Init(&g_chat_lock);
}

void config_load(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.dark = true;
    g_cfg.show_thoughts = true;
    g_cfg.auto_memory = true;
    g_cfg.mode = MODE_FAST;
    u8 *d = file_read(APP_DIR "/config.json", NULL);
    if (d) {
        cJSON *j = cJSON_Parse((char *)d);
        if (j) {
            SCPY(g_cfg.api_key, jstr(j, "api_key", ""));
            SCPY(g_cfg.name, jstr(j, "name", ""));
            g_cfg.mode = (int)jnum(j, "mode", MODE_FAST);
            SCPY(g_cfg.custom_model, jstr(j, "custom_model", ""));
            g_cfg.dark = jbool(j, "dark", true);
            g_cfg.text_size = (int)jnum(j, "text_size", TS_NORMAL);
            g_cfg.show_thoughts = jbool(j, "show_thoughts", true);
            g_cfg.auto_memory = jbool(j, "auto_memory", true);
            SCPY(g_cfg.saved_info, jstr(j, "saved_info", ""));
            SCPY(g_cfg.api_base, jstr(j, "api_base", ""));
            g_cfg.lang = (int)jnum(j, "lang", 0);
            cJSON *m = cJSON_GetObjectItem(j, "models");
            if (m) {
                SCPY(g_models.flash, jstr(m, "flash", ""));
                SCPY(g_models.pro, jstr(m, "pro", ""));
                SCPY(g_models.lite, jstr(m, "lite", ""));
            }
            cJSON_Delete(j);
        }
        free(d);
    }
    // importar clave desde apikey.txt si existe
    u8 *k = file_read(APP_DIR "/apikey.txt", NULL);
    if (k) {
        char *s = (char *)k;
        str_trim(s);
        if (strlen(s) >= 20 && strlen(s) < sizeof(g_cfg.api_key)) {
            SCPY(g_cfg.api_key, s);
            remove(APP_DIR "/apikey.txt");
            config_save();
        }
        free(k);
    }
}

void config_save(void) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "api_key", g_cfg.api_key);
    cJSON_AddStringToObject(j, "name", g_cfg.name);
    cJSON_AddNumberToObject(j, "mode", g_cfg.mode);
    cJSON_AddStringToObject(j, "custom_model", g_cfg.custom_model);
    cJSON_AddBoolToObject(j, "dark", g_cfg.dark);
    cJSON_AddNumberToObject(j, "text_size", g_cfg.text_size);
    cJSON_AddBoolToObject(j, "show_thoughts", g_cfg.show_thoughts);
    cJSON_AddBoolToObject(j, "auto_memory", g_cfg.auto_memory);
    cJSON_AddNumberToObject(j, "lang", g_cfg.lang);
    cJSON_AddStringToObject(j, "saved_info", g_cfg.saved_info);
    if (g_cfg.api_base[0]) cJSON_AddStringToObject(j, "api_base", g_cfg.api_base);
    cJSON *m = cJSON_AddObjectToObject(j, "models");
    cJSON_AddStringToObject(m, "flash", g_models.flash);
    cJSON_AddStringToObject(m, "pro", g_models.pro);
    cJSON_AddStringToObject(m, "lite", g_models.lite);
    char *s = cJSON_Print(j);
    if (s) { file_write(APP_DIR "/config.json", s, strlen(s)); free(s); }
    cJSON_Delete(j);
}

// ------------------------------------------------------------ índice de chats
static int meta_cmp(const void *a, const void *b) {
    const ChatMeta *x = a, *y = b;
    if (x->pinned != y->pinned) return x->pinned ? -1 : 1;
    return x->updated > y->updated ? -1 : (x->updated < y->updated ? 1 : 0);
}
void index_sort(void) { qsort(g_index, g_nindex, sizeof(ChatMeta), meta_cmp); }

void index_load(void) {
    g_nindex = 0;
    u8 *d = file_read(CHATS_DIR "/index.json", NULL);
    if (!d) return;
    cJSON *j = cJSON_Parse((char *)d);
    free(d);
    if (!cJSON_IsArray(j)) { cJSON_Delete(j); return; }
    cJSON *it;
    cJSON_ArrayForEach(it, j) {
        if (g_nindex >= 300) break;
        ChatMeta *m = &g_index[g_nindex];
        SCPY(m->id, jstr(it, "id", ""));
        if (!m->id[0]) continue;
        SCPY(m->title, jstr(it, "title", "Chat"));
        m->pinned = jbool(it, "pinned", false);
        m->updated = (u64)jnum(it, "updated", 0);
        g_nindex++;
    }
    cJSON_Delete(j);
    index_sort();
}

void index_save(void) {
    cJSON *j = cJSON_CreateArray();
    for (int i = 0; i < g_nindex; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", g_index[i].id);
        cJSON_AddStringToObject(o, "title", g_index[i].title);
        cJSON_AddBoolToObject(o, "pinned", g_index[i].pinned);
        cJSON_AddNumberToObject(o, "updated", (double)g_index[i].updated);
        cJSON_AddItemToArray(j, o);
    }
    char *s = cJSON_PrintUnformatted(j);
    if (s) { file_write(CHATS_DIR "/index.json", s, strlen(s)); free(s); }
    cJSON_Delete(j);
}

void index_touch(Chat *c) {
    int i;
    for (i = 0; i < g_nindex; i++) if (!strcmp(g_index[i].id, c->id)) break;
    if (i == g_nindex) {
        if (g_nindex >= 300) {   // eliminar el más antiguo no fijado
            index_sort();
            int k = g_nindex - 1;
            while (k >= 0 && g_index[k].pinned) k--;
            if (k < 0) return;
            chat_delete_file(g_index[k].id);
            memmove(&g_index[k], &g_index[k + 1], (g_nindex - k - 1) * sizeof(ChatMeta));
            g_nindex--;
            i = g_nindex;
        }
        g_nindex++;
    }
    ChatMeta *m = &g_index[i];
    SCPY(m->id, c->id);
    SCPY(m->title, c->title[0] ? c->title : "Nuevo chat");
    m->pinned = c->pinned;
    m->updated = c->updated;
    index_sort();
    index_save();
}

void index_remove(const char *id) {
    for (int i = 0; i < g_nindex; i++)
        if (!strcmp(g_index[i].id, id)) {
            memmove(&g_index[i], &g_index[i + 1], (g_nindex - i - 1) * sizeof(ChatMeta));
            g_nindex--;
            break;
        }
    index_save();
}

// ------------------------------------------------------------ chats
bool chat_save(Chat *c) {
    if (c->temporary || c->n == 0) return false;
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", c->id);
    cJSON_AddStringToObject(j, "title", c->title);
    cJSON_AddBoolToObject(j, "pinned", c->pinned);
    cJSON_AddBoolToObject(j, "titled", c->titled);
    cJSON_AddNumberToObject(j, "updated", (double)c->updated);
    cJSON *arr = cJSON_AddArrayToObject(j, "msgs");
    LightLock_Lock(&g_chat_lock);
    for (int i = 0; i < c->n; i++) {
        Msg *m = c->m[i];
        if (m->state == MST_STREAM) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "r", m->role);
        cJSON_AddStringToObject(o, "t", m->text ? m->text : "");
        if (m->thought && m->thlen) cJSON_AddStringToObject(o, "th", m->thought);
        if (m->sig) cJSON_AddStringToObject(o, "sig", m->sig);
        if (m->att) {
            cJSON_AddNumberToObject(o, "att", m->att);
            cJSON_AddStringToObject(o, "ap", m->att_path);
            cJSON_AddStringToObject(o, "am", m->att_mime);
        }
        cJSON_AddStringToObject(o, "mo", m->model);
        cJSON_AddNumberToObject(o, "st", m->state);
        cJSON_AddNumberToObject(o, "tm", (double)m->time);
        cJSON_AddItemToArray(arr, o);
    }
    LightLock_Unlock(&g_chat_lock);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s) return false;
    char path[200];
    snprintf(path, sizeof(path), CHATS_DIR "/%s.json", c->id);
    bool ok = file_write(path, s, strlen(s));
    free(s);
    index_touch(c);
    return ok;
}

Chat *chat_load(const char *id) {
    char path[200];
    snprintf(path, sizeof(path), CHATS_DIR "/%s.json", id);
    u8 *d = file_read(path, NULL);
    if (!d) return NULL;
    cJSON *j = cJSON_Parse((char *)d);
    free(d);
    if (!j) return NULL;
    Chat *c = calloc(1, sizeof(Chat));
    SCPY(c->id, jstr(j, "id", id));
    SCPY(c->title, jstr(j, "title", ""));
    c->pinned = jbool(j, "pinned", false);
    c->titled = jbool(j, "titled", true);
    c->updated = (u64)jnum(j, "updated", 0);
    cJSON *arr = cJSON_GetObjectItem(j, "msgs"), *o;
    cJSON_ArrayForEach(o, arr) {
        Msg *m = msg_new((int)jnum(o, "r", 0));
        msg_set_text(m, jstr(o, "t", ""));
        const char *th = jstr(o, "th", NULL);
        if (th) msg_append_thought(m, th, strlen(th));
        const char *sig = jstr(o, "sig", NULL);
        if (sig) m->sig = strdup(sig);
        m->att = (int)jnum(o, "att", 0);
        SCPY(m->att_path, jstr(o, "ap", ""));
        SCPY(m->att_mime, jstr(o, "am", ""));
        SCPY(m->model, jstr(o, "mo", ""));
        m->state = (int)jnum(o, "st", MST_DONE);
        m->time = (u64)jnum(o, "tm", 0);
        m->appear = 1;
        chat_push(c, m);
    }
    cJSON_Delete(j);
    return c;
}

void chat_delete_file(const char *id) {
    char path[200];
    snprintf(path, sizeof(path), CHATS_DIR "/%s.json", id);
    remove(path);
}

void history_clear_all(void) {
    for (int i = 0; i < g_nindex; i++) chat_delete_file(g_index[i].id);
    g_nindex = 0;
    index_save();
}

// ------------------------------------------------------------ información guardada
// Los recuerdos se guardan en g_cfg.saved_info, uno por línea.
int mem_count(void) {
    int n = 0;
    const char *p = g_cfg.saved_info;
    while (*p) {
        const char *e = strchr(p, '\n');
        if ((e ? e - p : (long)strlen(p)) > 0) n++;
        if (!e) break;
        p = e + 1;
    }
    return n;
}

static bool mem_span(int i, const char **start, int *len) {
    int k = 0;
    const char *p = g_cfg.saved_info;
    while (*p) {
        const char *e = strchr(p, '\n');
        int l = e ? (int)(e - p) : (int)strlen(p);
        if (l > 0) {
            if (k == i) { *start = p; *len = l; return true; }
            k++;
        }
        if (!e) break;
        p = e + 1;
    }
    return false;
}

bool mem_get(int i, char *out, int n) {
    const char *s; int l;
    if (!mem_span(i, &s, &l)) { if (n) out[0] = 0; return false; }
    if (l > n - 1) l = n - 1;
    memcpy(out, s, l); out[l] = 0;
    return true;
}

static void clean_line(char *s) {
    for (char *p = s; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
    str_trim(s);
}

void mem_add(const char *in) {
    char buf[600];
    SCPY(buf, in);
    clean_line(buf);
    if (!buf[0]) return;
    for (int i = 0, n = mem_count(); i < n; i++) {      // sin repetidos
        char cur[600];
        if (mem_get(i, cur, sizeof(cur)) && !strcasecmp(cur, buf)) return;
    }
    size_t l = strlen(g_cfg.saved_info);
    if (l + strlen(buf) + 2 >= sizeof(g_cfg.saved_info)) return;
    if (l && g_cfg.saved_info[l - 1] != '\n') strcat(g_cfg.saved_info, "\n");
    strcat(g_cfg.saved_info, buf);
    config_save();
}

static void mem_rebuild(int skip, int replace, const char *with) {
    char out[sizeof(g_cfg.saved_info)] = "";
    int n = mem_count();
    for (int i = 0; i < n; i++) {
        if (i == skip) continue;
        char line[600];
        if (i == replace) { SCPY(line, with); clean_line(line); if (!line[0]) continue; }
        else mem_get(i, line, sizeof(line));
        if (strlen(out) + strlen(line) + 2 >= sizeof(out)) break;
        if (out[0]) strcat(out, "\n");
        strcat(out, line);
    }
    SCPY(g_cfg.saved_info, out);
    config_save();
}

void mem_set(int i, const char *s) { mem_rebuild(-1, i, s); }
void mem_delete(int i) { mem_rebuild(i, -1, NULL); }
void mem_clear(void) { g_cfg.saved_info[0] = 0; config_save(); }

int mem_forget(const char *needle) {
    char nd[200]; SCPY(nd, needle); clean_line(nd);
    if (strlen(nd) < 3) return 0;
    int removed = 0;
    for (int i = mem_count() - 1; i >= 0; i--) {
        char line[600];
        mem_get(i, line, sizeof(line));
        // comparación sin distinguir mayúsculas (ASCII)
        bool hit = false;
        size_t nl = strlen(nd);
        for (const char *p = line; *p && !hit; p++) if (!strncasecmp(p, nd, nl)) hit = true;
        if (hit) { mem_delete(i); removed++; }
    }
    return removed;
}

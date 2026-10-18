/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - pantalla principal: conversación (arriba) y controles (abajo)
#include "app.h"
#include "ui.h"
#include "gemini.h"
#include "store.h"
#include "media.h"
#include "util.h"
#include "scenes.h"
#include "pair.h"
#include "usage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <dirent.h>
#include <time.h>
#include "cJSON.h"

// ------------------------------------------------------------ estado
enum { SK_NONE, SK_DRAWER, SK_MODELS, SK_TOOLS, SK_ACTIONS, SK_SETTINGS, SK_CHATOPT, SK_PHOTOS,
       SK_MEMORY, SK_MEMOPT, SK_ABOUT, SK_LANG, SK_USAGE };
enum { OV_NONE, OV_QR, OV_IMAGE, OV_CAMERA, OV_PHOTO, OV_INFO };
enum { DLG_DEL_CHAT = 1, DLG_LOGOUT, DLG_CLEAR, DLG_EXIT, DLG_MEM_DEL, DLG_MEM_CLEAR };

#define URL_GITHUB  "https://github.com/venomshine"
#define URL_DISCORD "https://discord.com/invite/yVKp8JwV4e"
#define URL_KOFI    "https://ko-fi.com/venomshine"
#define URL_GEMINI  "https://gemini.google.com"
#define URL_USAGE   "https://aistudio.google.com/usage"
#define ABOUT_TEXT \
    "Muchísimas gracias por usar Gemi3DS y por tu apoyo. Cada estrella en GitHub, cada mensaje en Discord y cada café " \
    "en Ko-fi me ayudan a seguir mejorándolo, y habrá futuras actualizaciones con más funciones.\n" \
    "¿Has encontrado un error o algo raro? Cuéntamelo por GitHub o Discord y lo corregiré lo antes posible.\n" \
    "Cliente no oficial que usa la API de Gemini con tu propia clave. Gemini es una marca de Google LLC; " \
    "este proyecto no está afiliado a Google."

static Sheet S;
static int s_ov;
static float s_ov_anim;
static char s_ov_text[3000], s_ov_cap[1200], s_ov_title[80];
static char s_ov_url[96];              // QR con enlace al servidor local: el texto completo va en s_ov_text
static Texture s_ov_tex, s_cam_tex;

static float s_scroll, s_scroll_target, s_content_h;
static bool s_follow = true;
static int s_sel = -1;                 // siempre hay un mensaje resaltado (si el chat no está vacío)
static float s_vel;                    // inercia del touchpad
static bool s_pad_drag, s_need_snap;
static float s_idle;                   // tiempo sin desplazar (para anclar)
static float s_step_acc;               // arrastre acumulado en chats que caben en pantalla
static float s_bar_anim;               // 0 = inicio (reloj en la esquina), 1 = chat (reloj + lápiz)
static float s_sel_glow;               // pequeña animación al cambiar de mensaje
static int s_last_sel = -1;

static int s_att; static char s_att_path[160], s_att_mime[32]; static Texture s_att_tex;
static bool s_recording;
static int s_dlg_arg;
static char s_opt_id[24];
static char s_search[64];
static char s_photos[200][160]; static int s_nphotos; static int s_photo_shown = -1;

#define TOP_HDR 24.0f
#define MSG_X 32.0f
#define MSG_W 356.0f
#define BUB_MAXW 286.0f
#define ANCHOR (TOP_HDR + 56.0f)
#define PAD_X 8.0f
#define PAD_Y 44.0f
#define PAD_W 304.0f
#define PAD_H 96.0f
#define ROW_Y 146.0f

static void fmt_tokens(int n, char *out, int sz);
static void open_usage(int keep_sel);

static float ts(void) { return g_cfg.text_size == TS_LARGE ? 0.58f : (g_cfg.text_size == TS_SMALL ? 0.45f : 0.51f); }
static u32 on_accent(void) { return T.dark ? col_hex(0x062E6F) : 0xFFFFFFFF; }

// minúsculas y sin tildes (á→a, Ñ→n…) para que la búsqueda no dependa de cómo se escriba
static int fold_text(const char *in, int inlen, char *out, int outsz) {
    static const char lat1[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";   // U+00E0..U+00FF
    int o = 0;
    for (int i = 0; i < inlen && in[i] && o < outsz - 1; i++) {
        unsigned char c = in[i];
        if (c == 0xC3 && i + 1 < inlen) {
            unsigned char d = (unsigned char)in[++i];
            int k = (d | 0x20) - 0xA0;                    // C3 80..BF -> U+00C0..U+00FF
            out[o++] = (k >= 0 && k < 32) ? lat1[k] : '?';
        } else out[o++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    out[o] = 0;
    return o;
}

static bool ci_contains(const char *hay, const char *needle) {
    static char h[600], n[200];
    fold_text(hay, (int)strlen(hay), h, sizeof(h));
    fold_text(needle, (int)strlen(needle), n, sizeof(n));
    return strstr(h, n) != NULL;
}

// coincidencias de la búsqueda: título o contenido de cada chat (se calcula al buscar)
static char s_match_id[300][24];
static int s_nmatch;
static bool search_hit(const ChatMeta *m) {
    if (ci_contains(m->title, s_search)) return true;
    for (int i = 0; i < s_nmatch; i++) if (!strcmp(s_match_id[i], m->id)) return true;
    return false;
}
static void search_compute(void) {
    static bool s_match[300];
    s_nmatch = 0;
    char nf[200];
    fold_text(s_search, (int)strlen(s_search), nf, sizeof(nf));
    for (int i = 0; i < g_nindex && i < 300; i++) {
        s_match[i] = ci_contains(g_index[i].title, s_search);
        if (s_match[i] || !nf[0]) continue;
        char path[200];
        snprintf(path, sizeof(path), CHATS_DIR "/%s.json", g_index[i].id);
        char *raw = (char *)file_read(path, NULL);
        if (!raw) continue;
        cJSON *j = cJSON_Parse(raw);
        free(raw);
        cJSON *msgs = j ? cJSON_GetObjectItem(j, "msgs") : NULL, *mm;
        static char fl[32768];
        cJSON_ArrayForEach(mm, msgs) {
            cJSON *t = cJSON_GetObjectItem(mm, "t");
            if (!cJSON_IsString(t)) continue;
            fold_text(t->valuestring, (int)strlen(t->valuestring), fl, sizeof(fl));
            if (strstr(fl, nf)) { s_match[i] = true; break; }
        }
        cJSON_Delete(j);
    }
    for (int i = 0; i < g_nindex && i < 300; i++)
        if (s_match[i]) snprintf(s_match_id[s_nmatch++], 24, "%s", g_index[i].id);
}

// ------------------------------------------------------------ desplazamiento y selección
static float max_scroll(void) { float m = s_content_h - SCR_H; return m < 0 ? 0 : m; }

static void scroll_to_bottom(void) {
    s_scroll_target = max_scroll();
    s_follow = true;
    s_vel = 0;
    if (g_chat && g_chat->n) s_sel = g_chat->n - 1;
}

// lleva el mensaje i a la parte superior de la vista (o lo más cerca posible)
static void scroll_to_msg(int i) {
    if (!g_chat || i < 0 || i >= g_chat->n) return;
    Msg *m = g_chat->m[i];
    s_scroll_target = m->y - TOP_HDR - 8;
    if (s_scroll_target > max_scroll()) s_scroll_target = max_scroll();
    if (s_scroll_target < 0) s_scroll_target = 0;
    s_follow = false;
}

// mensaje resaltado según el desplazamiento: la línea de anclaje recorre la pantalla
// proporcionalmente (arriba del todo = primer mensaje, abajo del todo = último)
static int msg_at_anchor(float scroll) {
    if (!g_chat || g_chat->n == 0) return -1;
    float ms = max_scroll();
    float frac = ms > 0 ? clampf(scroll / ms, 0, 1) : 0;
    float ay = scroll + TOP_HDR + 10 + (SCR_H - TOP_HDR - 20) * frac;
    int best = 0;
    float bestd = 1e9f;
    for (int i = 0; i < g_chat->n; i++) {
        Msg *m = g_chat->m[i];
        float d = ay < m->y ? m->y - ay : (ay > m->y + m->h ? ay - (m->y + m->h) : 0);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

static void select_msg(int i) {
    if (!g_chat || g_chat->n == 0) { s_sel = -1; return; }
    if (i < 0) i = 0;
    if (i >= g_chat->n) i = g_chat->n - 1;
    s_sel = i;
    scroll_to_msg(i);
}

// ------------------------------------------------------------ chats
static void clear_attachment(void) {
    s_att = ATT_NONE; s_att_path[0] = 0; s_att_mime[0] = 0;
    tex_free(&s_att_tex);
}

static void new_chat(bool temporary) {
    gem_stop_wait();
    if (g_chat && g_chat->n > 0) chat_save(g_chat);
    LightLock_Lock(&g_chat_lock);
    chat_free(g_chat);
    g_chat = chat_new();
    g_chat->temporary = temporary;
    LightLock_Unlock(&g_chat_lock);
    s_sel = -1; s_scroll = s_scroll_target = 0; s_follow = true; s_vel = 0;
    clear_attachment();
}

static void open_chat(const char *id) {
    if (g_chat && !strcmp(g_chat->id, id)) return;
    gem_stop_wait();
    Chat *c = chat_load(id);
    if (!c) { toast(_("No se pudo abrir el chat")); index_remove(id); return; }
    if (g_chat && g_chat->n > 0) chat_save(g_chat);
    LightLock_Lock(&g_chat_lock);
    chat_free(g_chat);
    g_chat = c;
    LightLock_Unlock(&g_chat_lock);
    s_content_h = 0;
    s_scroll = s_scroll_target = 1e9f;   // se ajusta al maquetar
    s_follow = true; s_vel = 0;
    s_sel = g_chat->n - 1;
    clear_attachment();
}

static void send_message(const char *text) {
    Msg *u = msg_new(ROLE_USER);
    msg_set_text(u, text);
    if (s_att) {
        u->att = s_att;
        snprintf(u->att_path, sizeof(u->att_path), "%s", s_att_path);
        snprintf(u->att_mime, sizeof(u->att_mime), "%s", s_att_mime);
    }
    Msg *m = msg_new(ROLE_MODEL);
    LightLock_Lock(&g_chat_lock);
    chat_push(g_chat, u);
    chat_push(g_chat, m);
    LightLock_Unlock(&g_chat_lock);
    if (!g_chat->title[0]) {
        char t[100];
        if (text[0]) {
            md_strip(text, t, 60);
            for (char *p = t; *p; p++) if (*p == '\n') *p = ' ';
            str_trim(t);
        } else snprintf(t, sizeof(t), "%s", u->att == ATT_AUDIO ? _("Nota de voz") : _("Foto"));
        snprintf(g_chat->title, sizeof(g_chat->title), "%s", t);
    }
    g_chat->updated = osGetTime();
    gem_send(g_chat, m);
    clear_attachment();
    s_follow = true;
    s_sel = g_chat->n - 1;
}

static void compose(const char *initial) {
    if (gem_streaming()) { toast(_("Espera a que termine la respuesta")); return; }
    char *buf = calloc(1, 4096);
    const char *hint = s_att == ATT_IMAGE ? _("Pregunta algo sobre la foto (o déjalo vacío)") : _("Pregúntale a Gemini");
    if (kb_input(hint, initial, buf, 4096, true, false)) {
        str_trim(buf);
        if (buf[0] || s_att) send_message(buf);
    }
    free(buf);
}

static void regenerate(int i) {
    if (gem_streaming() || i < 0 || i >= g_chat->n) return;
    if (g_chat->m[i]->role == ROLE_USER) i++;
    if (i >= g_chat->n || g_chat->m[i]->role != ROLE_MODEL) return;
    LightLock_Lock(&g_chat_lock);
    chat_truncate(g_chat, i);
    Msg *m = msg_new(ROLE_MODEL);
    chat_push(g_chat, m);
    LightLock_Unlock(&g_chat_lock);
    gem_send(g_chat, m);
    s_follow = true;
    s_sel = g_chat->n - 1;
}

static void edit_message(int i) {
    if (gem_streaming() || i < 0 || i >= g_chat->n) return;
    if (g_chat->m[i]->role == ROLE_MODEL) i--;
    if (i < 0 || g_chat->m[i]->role != ROLE_USER) return;
    Msg *u = g_chat->m[i];
    char *buf = calloc(1, 4096);
    if (kb_input(_("Edita tu mensaje"), u->text, buf, 4096, true, false)) {
        str_trim(buf);
        if (buf[0] || u->att) {
            int att = u->att; char ap[160], am[32];
            snprintf(ap, sizeof(ap), "%s", u->att_path); snprintf(am, sizeof(am), "%s", u->att_mime);
            LightLock_Lock(&g_chat_lock);
            chat_truncate(g_chat, i);
            LightLock_Unlock(&g_chat_lock);
            s_att = att; snprintf(s_att_path, sizeof(s_att_path), "%s", ap); snprintf(s_att_mime, sizeof(s_att_mime), "%s", am);
            send_message(buf);
        }
    }
    free(buf);
}

static void show_qr(const char *text, const char *title, const char *cap) {
    snprintf(s_ov_text, sizeof(s_ov_text), "%s", text);
    snprintf(s_ov_title, sizeof(s_ov_title), "%s", title);
    snprintf(s_ov_cap, sizeof(s_ov_cap), "%s", cap);
    s_ov_url[0] = 0;
    s_ov = OV_QR; s_ov_anim = 0;
}

// el mensaje se sirve desde la propia consola y el QR solo lleva la URL corta, que siempre
// se lee bien; sin Wi-Fi se vuelve al QR con el texto (recortado si no cabe)
static void share_qr(int i) {
    if (i < 0 || i >= g_chat->n) return;
    Msg *m = g_chat->m[i];
    int sz = m->tlen + 16;
    char *t = malloc(sz);
    if (!t) return;
    md_strip(m->text ? m->text : "", t, sz);
    char url[96];
    if (pair_share(t, url, sizeof(url))) {
        show_qr(t, _("Compartir mensaje"), _("Escanéalo con tu teléfono (en la misma Wi-Fi) para ver y copiar el texto completo."));
        snprintf(s_ov_url, sizeof(s_ov_url), "%s", url);
        free(t);
        return;
    }
    int len = strlen(t);
    bool cut = false;
    if (len > 1300) { len = 1300; while (len > 0 && ((u8)t[len] & 0xC0) == 0x80) len--; t[len] = 0; cut = true; }
    while (len > 40 && qr_size_for(t) == 0) {
        len = len * 85 / 100;
        while (len > 0 && ((u8)t[len] & 0xC0) == 0x80) len--;
        t[len] = 0; cut = true;
    }
    show_qr(t, _("Compartir mensaje"), cut ? _("Escanéalo con tu teléfono para copiar el texto (recortado).") : _("Escanéalo con tu teléfono para copiar el texto."));
    free(t);
}

static void show_info(const char *title, const char *text, const char *qr) {
    snprintf(s_ov_title, sizeof(s_ov_title), "%s", title);
    snprintf(s_ov_cap, sizeof(s_ov_cap), "%s", text);
    snprintf(s_ov_text, sizeof(s_ov_text), "%s", qr ? qr : "");
    s_ov = OV_INFO; s_ov_anim = 0;
}

static void explain_no_images(void) {
    show_info(_("Crear imágenes"),
        _("La 3DS no puede crear imágenes con Gemini: generarlas y mostrarlas requiere mucha más memoria, potencia y "
        "resolución de pantalla de las que tiene la consola. Para crear imágenes, usa Gemini en el navegador o en la "
        "app de Gemini para Android o iPhone. Escanea el código QR para abrirlo en tu teléfono."), URL_GEMINI);
}

static void save_txt(int i) {
    Msg *m = g_chat->m[i];
    char id[24], path[160];
    make_id(id, sizeof(id));
    snprintf(path, sizeof(path), MEDIA_DIR "/respuesta_%s.txt", id);
    if (file_write(path, m->text, m->tlen)) {
        char t[200]; snprintf(t, sizeof(t), _("Guardado en %s"), path + 5);
        toast(t);
    } else toast(_("No se pudo guardar"));
}

static void view_image(const char *path, const char *cap) {
    tex_free(&s_ov_tex);
    if (!img_load_tex(path, &s_ov_tex, 400, 240)) { toast(_("No se pudo abrir la imagen")); return; }
    snprintf(s_ov_cap, sizeof(s_ov_cap), "%s", cap);
    s_ov = OV_IMAGE; s_ov_anim = 0;
}

// ------------------------------------------------------------ fotos de la SD
static void scan_dir(const char *dir, int depth) {
    if (depth > 2 || s_nphotos >= 200) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_nphotos < 200) {
        if (e->d_name[0] == '.') continue;
        char p[160];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        const char *ext = strrchr(e->d_name, '.');
        if (e->d_type == DT_DIR) scan_dir(p, depth + 1);
        else if (ext && (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png") || !strcasecmp(ext, ".mpo")))
            snprintf(s_photos[s_nphotos++], 160, "%s", p);
    }
    closedir(d);
}
static int cmp_desc(const void *a, const void *b) { return -strcmp((const char *)a, (const char *)b); }

// ------------------------------------------------------------ hojas
static void open_drawer(void) {
    sheet_open(&S, SK_DRAWER, APP_NAME, true);
    sheet_add(&S, IC_EDIT, _("Nueva conversación"), NULL, SR_NONE, -10);
    char sub[80] = "";
    if (s_search[0]) snprintf(sub, sizeof(sub), _("Filtro: \"%s\" (toca para cambiar)"), s_search);
    sheet_add(&S, IC_SEARCH, _("Buscar chats"), sub, s_search[0] ? SR_CLEAR : SR_NONE, -11);
    sheet_header(&S, s_search[0] ? _("Resultados") : _("Recientes"));
    int shown = 0;
    for (int i = 0; i < g_nindex; i++) {
        ChatMeta *m = &g_index[i];
        if (s_search[0] && !search_hit(m)) continue;
        char when[40];
        fmt_when(m->updated, when, sizeof(when));
        SheetItem *it = sheet_add(&S, m->pinned ? IC_PIN : IC_CHAT, m->title, when, (g_chat && !strcmp(g_chat->id, m->id)) ? SR_CHECK : SR_NONE, i);
        if (m->pinned) it->icon_col = T.accent;
        it->swipe = true; it->pinned = m->pinned;
        shown++;
    }
    if (!shown) {
        SheetItem *it = sheet_add(&S, -1, s_search[0] ? _("Sin resultados") : _("Aún no tienes conversaciones"), NULL, SR_NONE, -99);
        it->disabled = true;
    }
    sheet_header(&S, "");
    sheet_add(&S, IC_GEAR, _("Ajustes"), NULL, SR_CHEV, -13);
    S.sel = 0;
}

static void open_models(void) {
    sheet_open(&S, SK_MODELS, _("Elige un modelo"), false);
    char sub[160];
    sheet_add(&S, IC_BOLT, _("Rápido"), _("Respuestas rápidas · cambia solo si uno se satura"), g_cfg.mode == MODE_FAST ? SR_CHECK : SR_NONE, MODE_FAST);
    sheet_add(&S, IC_BULB, _("Razonamiento"), _("Piensa antes de responder · modelos Flash"), g_cfg.mode == MODE_THINK ? SR_CHECK : SR_NONE, MODE_THINK);
    if (g_models.pro[0]) snprintf(sub, sizeof(sub), _("%s · puede requerir facturación"), g_models.pro);
    else snprintf(sub, sizeof(sub), _("No disponible con tu clave"));
    SheetItem *p = sheet_add(&S, IC_SPARK, "Pro", sub, g_cfg.mode == MODE_PRO ? SR_CHECK : SR_NONE, MODE_PRO);
    p->disabled = !g_models.pro[0];
    sheet_header(&S, _("Todos los modelos (se usa exactamente el elegido)"));
    for (int i = 0; i < g_models.n; i++) {
        ModelInfo *mi = &g_models.list[i];
        bool cur = g_cfg.mode == MODE_CUSTOM && !strcmp(g_cfg.custom_model, mi->id);
        sheet_add(&S, IC_SPARK, mi->name, mi->id, cur ? SR_CHECK : SR_NONE, 100 + i);
    }
    sheet_add(&S, IC_REFRESH, gem_models_busy() ? _("Cargando modelos…") : _("Actualizar lista de modelos"), NULL, SR_NONE, 99);
    for (int i = 0; i < S.n; i++) if (S.items[i].right == SR_CHECK) { S.sel = i; break; }
}

static void open_tools(void) {
    sheet_open(&S, SK_TOOLS, _("Añadir"), false);
    sheet_add(&S, IC_CAMERA, _("Cámara"), _("Haz una foto y pregunta sobre ella"), SR_NONE, 1);
    sheet_add(&S, IC_IMAGE, _("Fotos de la consola"), _("Elige una imagen de la tarjeta SD"), SR_CHEV, 2);
    sheet_add(&S, IC_MIC, _("Nota de voz"), _("Habla y Gemini te responde"), SR_NONE, 3);
    SheetItem *c = sheet_add(&S, IC_SPARK, _("Crear imagen"), _("No disponible en 3DS, toca para saber más"), SR_CHEV, 4);
    c->icon_col = T.text3;
}

static void open_settings(int keep_sel) {
    float sc = S.scroll, sct = S.scroll_target;
    sheet_open(&S, SK_SETTINGS, _("Ajustes"), false);
    sheet_header(&S, _("Cuenta"));
    SheetItem *it = sheet_add(&S, IC_USER, _("Tu nombre"), NULL, SR_VALUE, 1);
    snprintf(it->value, sizeof(it->value), "%s", g_cfg.name[0] ? g_cfg.name : "—");
    char sub[140];
    int nm = mem_count();
    if (nm) snprintf(sub, sizeof(sub), _("%d %s · ver, editar o borrar"), nm, nm == 1 ? _("dato guardado") : _("datos guardados"));
    else snprintf(sub, sizeof(sub), _("Cosas que Gemini debe recordar de ti"));
    sheet_add(&S, IC_NOTE, _("Información guardada"), sub, SR_CHEV, 2);
    sheet_add(&S, IC_KEY, _("Cambiar clave / cerrar sesión"), _("Vuelve a vincular con el QR"), SR_CHEV, 3);
    sheet_header(&S, _("Apariencia"));
    it = sheet_add(&S, IC_GLOBE, _("Idioma"), NULL, SR_VALUE, 7);
    snprintf(it->value, sizeof(it->value), "%s", lang_setting_name(g_cfg.lang));
    it = sheet_add(&S, IC_PALETTE, _("Tema"), NULL, SR_VALUE, 4);
    snprintf(it->value, sizeof(it->value), "%s", g_cfg.dark ? _("Oscuro") : _("Claro"));
    it = sheet_add(&S, IC_TEXTSIZE, _("Tamaño del texto"), NULL, SR_VALUE, 5);
    snprintf(it->value, sizeof(it->value), "%s", g_cfg.text_size == TS_LARGE ? _("Grande") : (g_cfg.text_size == TS_SMALL ? _("Pequeño") : _("Normal")));
    sheet_header(&S, _("Respuestas"));
    sheet_add(&S, IC_BULB, _("Mostrar razonamiento"), _("En modo Razonamiento y Pro"), g_cfg.show_thoughts ? SR_ON : SR_OFF, 6);
    {
        int ur, ut; usage_totals(&ur, &ut);
        char tk[16]; fmt_tokens(ut, tk, sizeof(tk));
        if (ur) snprintf(sub, sizeof(sub), _("Hoy: %d %s · %s tokens"), ur, ur == 1 ? _("petición") : _("peticiones"), tk);
        else snprintf(sub, sizeof(sub), _("Peticiones, tokens y límites de cada modelo"));
        sheet_add(&S, IC_BOLT, _("Uso de los modelos"), sub, SR_CHEV, 13);
    }
    snprintf(sub, sizeof(sub), _("%d modelos disponibles con tu clave"), g_models.n);
    sheet_add(&S, IC_REFRESH, gem_models_busy() ? _("Actualizando modelos…") : _("Actualizar lista de modelos"), sub, SR_NONE, 10);
    if (g_nindex > 0) {
        sheet_header(&S, _("Datos"));
        it = sheet_add(&S, IC_TRASH, _("Borrar todo el historial"), NULL, SR_NONE, 11);
        it->icon_col = T.danger;
    }
    sheet_header(&S, _("Información"));
    snprintf(sub, sizeof(sub), _("Versión %s · por %s"), APP_VERSION, APP_AUTHOR);
    sheet_add(&S, IC_INFO, _("Acerca de " APP_NAME), sub, SR_CHEV, 12);
    if (keep_sel >= 0) { S.sel = keep_sel; S.scroll = sc; S.scroll_target = sct; S.anim = 1; }
}

// 1234 -> "1234", 12345 -> "12,3k" (12.3k en inglés y japonés), 1234567 -> "1,2M"
static void fmt_tokens(int n, char *out, int sz) {
    char dec = g_lang == LANG_ES ? ',' : '.';
    if (n < 10000) snprintf(out, sz, "%d", n);
    else if (n < 1000000) snprintf(out, sz, "%d%c%dk", n / 1000, dec, (n / 100) % 10);
    else snprintf(out, sz, "%d%c%dM", n / 1000000, dec, (n / 100000) % 10);
}

static const char *model_name(const char *id) {
    for (int i = 0; i < g_models.n; i++) if (!strcmp(g_models.list[i].id, id)) return g_models.list[i].name;
    return id;
}

// estado de un modelo para la lista de uso
static const char *usage_state(const UsageModel *u, u32 *col) {
    *col = T.accent;
    if (!strcmp(gem_active_model(), u->id)) return _("En uso");
    if (u->limit_day && u->req >= u->limit_day) { *col = T.danger; return _("Sin cupo"); }
    if (u->last_code == 429 && osGetTime() - u->last_time < 60000) { *col = T.danger; return _("Límite"); }
    if (gem_model_paused(u->id)) { *col = T.text3; return _("En pausa"); }
    if (u->last_code >= 500) { *col = T.text3; return _("Saturado"); }
    *col = T.ok;
    return "OK";
}

static float s_usage_refresh;

// uso de los modelos: se rehace cada segundo para que los datos estén al día
static void open_usage(int keep_sel) {
    float sc = S.scroll, sct = S.scroll_target;
    sheet_open(&S, SK_USAGE, _("Uso de los modelos"), false);
    S.anim = 1;
    s_usage_refresh = 0;
    int n = usage_count(), shown = 0;
    sheet_header(&S, _("Hoy, contado en esta consola"));
    for (int i = 0; i < n; i++) {
        UsageModel *u = usage_get(i);
        if (!u->req && strcmp(u->id, gem_current_model())) continue;
        char sub[140], tk[16];
        fmt_tokens(u->tok_in + u->tok_out, tk, sizeof(tk));
        int rpm = usage_rpm(u);
        if (u->limit_day) snprintf(sub, sizeof(sub), _("%d/%d hoy · %d/min · %s tokens"), u->req, u->limit_day, rpm, tk);
        else snprintf(sub, sizeof(sub), _("%d hoy · %d/min · %s tokens"), u->req, rpm, tk);
        SheetItem *it = sheet_add(&S, IC_SPARK, model_name(u->id), sub, SR_VALUE, i);
        u32 c; snprintf(it->value, sizeof(it->value), "%s", usage_state(u, &c));
        it->icon_col = c;
        shown++;
    }
    if (!shown) {
        SheetItem *e = sheet_add(&S, -1, _("Aún no hay peticiones hoy"), _("Escribe a Gemini y aparecerán aquí"), SR_NONE, -9);
        e->disabled = true;
    }
    sheet_header(&S, "");
    sheet_add(&S, IC_QR, _("Ver el uso oficial en AI Studio"), "aistudio.google.com/usage", SR_CHEV, -2);
    if (keep_sel >= 0 && keep_sel < S.n) { S.sel = keep_sel; S.scroll = sc; S.scroll_target = sct; }
    else for (int i = 0; i < S.n; i++) if (!S.items[i].header && !S.items[i].disabled) { S.sel = i; break; }
}

static void open_lang(void) {
    sheet_open(&S, SK_LANG, _("Idioma"), false);
    for (int l = LSET_SYSTEM; l <= LSET_JA; l++)
        sheet_add(&S, l == LSET_SYSTEM ? IC_GEAR : IC_GLOBE, lang_setting_name(l),
                  l == LSET_SYSTEM ? _("Usa el idioma de la consola") : NULL, g_cfg.lang == l ? SR_CHECK : SR_NONE, l);
    S.sel = g_cfg.lang;
    S.anim = 1;
}

static void open_about(void) {
    sheet_open(&S, SK_ABOUT, _("Acerca de " APP_NAME), false);
    S.anim = 1;
    sheet_header(&S, _("Creado por " APP_AUTHOR " · elige uno para ver su QR"));
    SheetItem *a = sheet_add(&S, IC_CODE, "GitHub", "github.com/venomshine", SR_CHEV, 1);
    a->icon_col = T.text;
    SheetItem *b = sheet_add(&S, IC_CHAT, "Discord", _("Únete a la comunidad"), SR_CHEV, 2);
    b->icon_col = col_hex(0x8C9EFF);
    SheetItem *c = sheet_add(&S, IC_STAR, "Ko-fi", _("Invítame a un café"), SR_CHEV, 3);
    c->icon_col = col_hex(0xFF8A80);
    sheet_header(&S, "");
    SheetItem *v = sheet_add(&S, IC_SPARK, "Gemi3DS " APP_VERSION, _("Gracias por usarlo"), SR_NONE, 4);
    v->icon_col = T.accent;
    S.sel = 4;
}

static void open_memory(int keep_sel) {
    float sc = S.scroll;
    sheet_open(&S, SK_MEMORY, _("Información guardada"), false);
    S.anim = 1;
    SheetItem *a = sheet_add(&S, IC_PLUS, _("Añadir información"), _("Algo que Gemini debe saber de ti"), SR_NONE, -1);
    a->icon_col = T.accent;
    int n = mem_count();
    char h[60]; snprintf(h, sizeof(h), _("Lo que Gemini recuerda (%d)"), n);
    sheet_header(&S, h);
    if (!n) {
        SheetItem *e = sheet_add(&S, -1, _("Aún no hay nada guardado"), _("Dile a Gemini \"recuerda que…\" o añádelo aquí"), SR_NONE, -9);
        e->disabled = true;
    }
    for (int i = 0; i < n; i++) {
        char line[600];
        mem_get(i, line, sizeof(line));
        sheet_add(&S, IC_NOTE, line, NULL, SR_CHEV, i);
    }
    if (n) {
        sheet_header(&S, "");
        SheetItem *d = sheet_add(&S, IC_TRASH, _("Borrar toda la información"), NULL, SR_NONE, -2);
        d->icon_col = T.danger;
    }
    if (keep_sel >= 0 && keep_sel < S.n) { S.sel = keep_sel; S.scroll = S.scroll_target = sc; }
}

static void open_actions(int i) {
    if (i < 0 || i >= g_chat->n) return;
    Msg *m = g_chat->m[i];
    sheet_open(&S, SK_ACTIONS, m->role == ROLE_USER ? _("Tu mensaje") : _("Respuesta de Gemini"), false);
    S.ctx = i;
    if (m->role == ROLE_MODEL) {
        sheet_add(&S, IC_REFRESH, _("Otra respuesta"), _("Vuelve a generar esta respuesta"), SR_NONE, 1);
        if (m->thlen) sheet_add(&S, IC_BULB, m->show_thought ? _("Ocultar razonamiento") : _("Mostrar razonamiento"), NULL, SR_NONE, 5);
    } else {
        sheet_add(&S, IC_EDIT, _("Editar y volver a enviar"), NULL, SR_NONE, 6);
    }
    if (m->att == ATT_IMAGE) sheet_add(&S, IC_IMAGE, _("Ver foto"), m->att_path + 5, SR_CHEV, 7);
    if (m->tlen) {
        sheet_add(&S, IC_QR, _("Compartir por QR"), _("Pásalo a tu teléfono"), SR_CHEV, 3);
        sheet_add(&S, IC_NOTE, _("Guardar como texto en la SD"), NULL, SR_NONE, 8);
    }
}

static void open_photos(void) {
    s_nphotos = 0;
    scan_dir("sdmc:/DCIM", 0);
    scan_dir(MEDIA_DIR, 1);
    qsort(s_photos, s_nphotos, 160, cmp_desc);
    sheet_open(&S, SK_PHOTOS, _("Fotos de la consola"), false);
    for (int i = 0; i < s_nphotos; i++) {
        const char *nm = strrchr(s_photos[i], '/');
        sheet_add(&S, IC_IMAGE, nm ? nm + 1 : s_photos[i], s_photos[i] + 5, SR_NONE, i);
    }
    if (!s_nphotos) { SheetItem *it = sheet_add(&S, -1, _("No hay fotos en la tarjeta SD"), _("Haz fotos con la cámara de la 3DS"), SR_NONE, -1); it->disabled = true; }
    s_photo_shown = -1;
}

// ------------------------------------------------------------ micrófono y cámara
static void start_mic(void) {
    if (gem_streaming()) { toast(_("Espera a que termine la respuesta")); return; }
    if (!mic_start()) { toast(_("No se pudo usar el micrófono")); return; }
    s_recording = true;
}

static void finish_mic(bool send) {
    mic_stop();
    s_recording = false;
    if (!send) return;
    char path[160];
    if (!mic_save_wav(path, sizeof(path))) { toast(_("La grabación es demasiado corta")); return; }
    clear_attachment();
    s_att = ATT_AUDIO;
    snprintf(s_att_path, sizeof(s_att_path), "%s", path);
    snprintf(s_att_mime, sizeof(s_att_mime), "audio/wav");
    send_message("");
}

static void open_camera(void) {
    if (!cam_open(false)) { toast(_("No se pudo abrir la cámara")); return; }
    s_ov = OV_CAMERA; s_ov_anim = 0;
}

static void attach_image(const char *path) {
    clear_attachment();
    s_att = ATT_IMAGE;
    snprintf(s_att_path, sizeof(s_att_path), "%s", path);
    const char *ext = strrchr(path, '.');
    snprintf(s_att_mime, sizeof(s_att_mime), "%s", ext && !strcasecmp(ext, ".png") ? "image/png" : "image/jpeg");
    img_load_tex(path, &s_att_tex, 96, 64);
}

// vuelve a abrir el cajón conservando la posición (tras fijar o renombrar)
static void reopen_drawer(void) {
    bool was = S.open && S.kind == SK_DRAWER;
    float sc = S.scroll; int sel = S.sel;
    open_drawer(); S.anim = 1;
    if (was) { S.scroll = S.scroll_target = sc; if (sel < S.n) S.sel = sel; }
}

// fijar (1), cambiar nombre (2) o eliminar (3) el chat s_opt_id: desde el menú o deslizando
static void chat_option(int action) {
    int ci = -1;
    for (int k = 0; k < g_nindex; k++) if (!strcmp(g_index[k].id, s_opt_id)) ci = k;
    if (ci < 0) { sheet_close(&S); return; }
    ChatMeta *cm = &g_index[ci];
    if (action == SW_PIN) {
        bool cur = g_chat && !strcmp(g_chat->id, cm->id);
        Chat *c = cur ? g_chat : chat_load(cm->id);
        bool pinned = false;
        if (c) { c->pinned = !c->pinned; pinned = c->pinned; chat_save(c); if (!cur) chat_free(c); }
        toast(pinned ? _("Chat fijado") : _("Chat desfijado"));
        reopen_drawer();
    } else if (action == SW_RENAME) {
        char t[100] = "";
        if (kb_input(_("Nuevo nombre del chat"), cm->title, t, sizeof(t), false, false) && t[0]) {
            bool cur = g_chat && !strcmp(g_chat->id, cm->id);
            Chat *c = cur ? g_chat : chat_load(cm->id);
            if (c) { snprintf(c->title, sizeof(c->title), "%s", t); c->titled = true; chat_save(c); if (!cur) chat_free(c); }
        }
        reopen_drawer();
    } else if (action == SW_DELETE) {
        char m[200]; snprintf(m, sizeof(m), _("Se eliminará \"%.60s\" de forma permanente."), cm->title);
        dialog_open(DLG_DEL_CHAT, _("¿Eliminar chat?"), m, _("Eliminar"), true);
    }
}

static void handle_sheet_swipe(int idx, int action) {
    SheetItem *it = &S.items[idx];
    if (S.kind != SK_DRAWER || it->id < 0 || it->id >= g_nindex) return;
    snprintf(s_opt_id, sizeof(s_opt_id), "%s", g_index[it->id].id);
    chat_option(action);
}

// ------------------------------------------------------------ elección en hojas
static void handle_sheet_choice(int idx) {
    SheetItem *it = &S.items[idx];
    int id = it->id;
    switch (S.kind) {
    case SK_DRAWER:
        if (id == -10) { new_chat(false); sheet_close(&S); }
        else if (id == -11 && s_search[0] && (S.hit_right || (g_kdown & KEY_Y))) {
            s_search[0] = 0;              // la X quita el filtro
            open_drawer(); S.anim = 1;
        } else if (id == -11) {
            char q[64] = "";
            kb_input(_("Buscar en tus chats"), s_search, q, sizeof(q), false, false);
            str_trim(q);
            snprintf(s_search, sizeof(s_search), "%s", q);
            search_compute();
            open_drawer();
            S.anim = 1;
        } else if (id == -13) open_settings(-1);
        else if (id >= 0 && id < g_nindex) { open_chat(g_index[id].id); sheet_close(&S); }
        break;
    case SK_CHATOPT:
        chat_option(id);
        break;
    case SK_MODELS:
        if (id == 99) { gem_models_fetch(); toast(_("Actualizando la lista de modelos…")); }
        else if (id >= 100) {
            g_cfg.mode = MODE_CUSTOM;
            snprintf(g_cfg.custom_model, sizeof(g_cfg.custom_model), "%s", g_models.list[id - 100].id);
            config_save(); sheet_close(&S);
        } else { g_cfg.mode = id; config_save(); sheet_close(&S); }
        break;
    case SK_TOOLS:
        if (id == 1) { sheet_close(&S); open_camera(); }
        else if (id == 2) open_photos();
        else if (id == 3) { sheet_close(&S); start_mic(); }
        else if (id == 4) { sheet_close(&S); explain_no_images(); }
        break;
    case SK_PHOTOS:
        if (id >= 0) { attach_image(s_photos[id]); sheet_close(&S); s_ov = OV_NONE; compose(NULL); }
        break;
    case SK_ACTIONS: {
        int mi = S.ctx;
        sheet_close(&S);
        if (mi < 0 || mi >= g_chat->n) break;
        Msg *m = g_chat->m[mi];
        if (id == 1) regenerate(mi);
        else if (id == 3) share_qr(mi);
        else if (id == 5) m->show_thought = !m->show_thought;
        else if (id == 6) edit_message(mi);
        else if (id == 7) view_image(m->att_path, m->att_path + 5);
        else if (id == 8) save_txt(mi);
        break;
    }
    case SK_SETTINGS: {
        int keep = idx;
        if (id == 1) {
            char n[48] = "";
            if (kb_input(_("¿Cómo quieres que te llame?"), g_cfg.name, n, sizeof(n), false, false)) {
                str_trim(n); snprintf(g_cfg.name, sizeof(g_cfg.name), "%s", n); config_save();
            }
        } else if (id == 2) { open_memory(-1); return; }
        else if (id == 3) {
            dialog_open(DLG_LOGOUT, _("¿Cerrar sesión?"), _("Se borrará la clave de esta consola. Tus chats se conservan. Tendrás que vincular de nuevo con el QR."), _("Cerrar sesión"), true);
        } else if (id == 4) { g_cfg.dark = !g_cfg.dark; theme_set(g_cfg.dark); config_save(); }
        else if (id == 5) { g_cfg.text_size = (g_cfg.text_size + 1) % 3; config_save(); }
        else if (id == 6) { g_cfg.show_thoughts = !g_cfg.show_thoughts; config_save(); }
        else if (id == 7) { open_lang(); return; }
        else if (id == 10) { gem_models_fetch(); toast(_("Actualizando la lista de modelos…")); }
        else if (id == 11) dialog_open(DLG_CLEAR, _("¿Borrar todo el historial?"), _("Se eliminarán todas tus conversaciones guardadas. No se puede deshacer."), _("Borrar todo"), true);
        else if (id == 12) { open_about(); return; }
        else if (id == 13) { open_usage(-1); return; }
        if (S.open && S.kind == SK_SETTINGS && !g_dialog.open) open_settings(keep);
        break;
    }
    case SK_LANG:
        g_cfg.lang = id;
        i18n_apply(id);
        config_save();
        open_settings(-1);
        S.anim = 1;
        toast(_("Idioma cambiado"));
        break;
    case SK_ABOUT:
        break;   // el QR se muestra arriba al resaltar cada enlace
    case SK_USAGE:
        break;   // los detalles (o el QR de AI Studio) se muestran arriba al resaltar
    case SK_MEMORY:
        if (id == -1) {
            char *b = calloc(1, 600);
            if (kb_input(_("¿Qué debe recordar Gemini? (p. ej. \"Soy vegetariano\")"), "", b, 600, false, false)) {
                str_trim(b);
                if (b[0]) { mem_add(b); toast(_("Guardado en tu información")); }
            }
            free(b);
            open_memory(idx);
        } else if (id == -2) {
            dialog_open(DLG_MEM_CLEAR, _("¿Borrar toda la información?"), _("Gemini olvidará todo lo que tiene guardado sobre ti."), _("Borrar"), true);
        } else if (id >= 0) {
            sheet_open(&S, SK_MEMOPT, _("Dato guardado"), false);
            S.ctx = id; S.anim = 1;
            sheet_add(&S, IC_EDIT, _("Editar"), NULL, SR_NONE, 1);
            SheetItem *d = sheet_add(&S, IC_TRASH, _("Eliminar"), NULL, SR_NONE, 2);
            d->icon_col = T.danger;
        }
        break;
    case SK_MEMOPT: {
        int mi = S.ctx;
        if (id == 1) {
            char cur[600], *b = calloc(1, 600);
            mem_get(mi, cur, sizeof(cur));
            if (kb_input(_("Edita el dato guardado"), cur, b, 600, false, false)) {
                str_trim(b);
                if (b[0]) mem_set(mi, b); else mem_delete(mi);
            }
            free(b);
            open_memory(-1);
        } else if (id == 2) {
            s_dlg_arg = mi;
            dialog_open(DLG_MEM_DEL, _("¿Eliminar este dato?"), _("Gemini dejará de recordarlo."), _("Eliminar"), true);
        }
        break;
    }
    }
}

static void handle_sheet_hold(int idx) {
    SheetItem *it = &S.items[idx];
    if (S.kind == SK_DRAWER && it->id >= 0 && it->id < g_nindex) {
        int ci = it->id;
        snprintf(s_opt_id, sizeof(s_opt_id), "%s", g_index[ci].id);
        sheet_open(&S, SK_CHATOPT, g_index[ci].title, false);
        S.anim = 0.4f;
        sheet_add(&S, IC_PIN, g_index[ci].pinned ? _("No fijar") : _("Fijar"), NULL, SR_NONE, 1);
        sheet_add(&S, IC_PEN, _("Cambiar nombre"), NULL, SR_NONE, 2);
        SheetItem *d = sheet_add(&S, IC_TRASH, _("Eliminar"), NULL, SR_NONE, 3);
        d->icon_col = T.danger;
    } else if (S.kind == SK_MEMORY && it->id >= 0) {
        handle_sheet_choice(idx);
    }
}

static void handle_dialog(int ctx, bool ok) {
    if (!ok) {
        if (ctx == DLG_MEM_DEL) open_memory(-1);
        return;
    }
    if (ctx == DLG_DEL_CHAT) {
        if (s_opt_id[0]) {
            char id[24]; snprintf(id, sizeof(id), "%s", s_opt_id);
            bool cur = g_chat && !strcmp(g_chat->id, id);
            if (cur) { gem_stop_wait(); LightLock_Lock(&g_chat_lock); chat_free(g_chat); g_chat = chat_new(); LightLock_Unlock(&g_chat_lock); s_sel = -1; }
            chat_delete_file(id);
            index_remove(id);
            toast(_("Chat eliminado"));
        }
        reopen_drawer();
    } else if (ctx == DLG_LOGOUT) {
        gem_stop_wait();
        if (g_chat->n) chat_save(g_chat);
        g_cfg.api_key[0] = 0;
        config_save();
        sheet_close(&S);
        scene_goto(SC_LOGIN);
    } else if (ctx == DLG_CLEAR) {
        gem_stop_wait();
        history_clear_all();
        LightLock_Lock(&g_chat_lock); chat_free(g_chat); g_chat = chat_new(); LightLock_Unlock(&g_chat_lock);
        s_sel = -1;
        toast(_("Historial borrado"));
        open_settings(-1);
    } else if (ctx == DLG_MEM_DEL) {
        mem_delete(s_dlg_arg);
        toast(_("Dato eliminado"));
        open_memory(-1);
    } else if (ctx == DLG_MEM_CLEAR) {
        mem_clear();
        toast(_("Información borrada"));
        open_memory(-1);
    } else if (ctx == DLG_EXIT) {
        g_quit = true;
    }
}

// ------------------------------------------------------------ ciclo
void chat_enter(void) {
    if (!g_chat) g_chat = chat_new();
    if (!g_models.loaded && !gem_models_busy()) gem_models_fetch();
}

void chat_shutdown(void) {
    gem_stop_wait();
    if (g_chat && g_chat->n > 0) chat_save(g_chat);
}

static void update_overlay_from_sheet(void) {
    if (!S.open || S.closing) {
        if (s_ov == OV_PHOTO) { s_ov = OV_NONE; tex_free(&s_ov_tex); }
        return;
    }
    if (S.kind == SK_PHOTOS) {
        SheetItem *it = sheet_selected(&S);
        if (it && it->id >= 0 && it->id != s_photo_shown && !g_touch.held) {
            s_photo_shown = it->id;
            tex_free(&s_ov_tex);
            if (img_load_tex(s_photos[it->id], &s_ov_tex, 400, 240)) { s_ov = OV_PHOTO; snprintf(s_ov_cap, sizeof(s_ov_cap), "%s", s_photos[it->id] + 5); }
        }
    }
}

// desplazamiento: touchpad (con inercia y anclaje), Circle Pad, ZL/ZR y ↑↓ por mensajes
static void update_scroll_input(void) {
    bool moved = false;
    bool in_pad = g_touch.sx >= PAD_X && g_touch.sx < PAD_X + PAD_W && g_touch.sy >= PAD_Y && g_touch.sy < PAD_Y + PAD_H;
    if (g_touch.held && g_touch.drag && in_pad && max_scroll() < 1 && g_chat->n > 0) {
        // el chat cabe en pantalla: deslizar cambia de mensaje cada ~32 px
        s_step_acc += (float)(g_touch.y - g_touch.py);
        if (s_step_acc > 32) { s_step_acc = 0; if (s_sel > 0) s_sel--; s_follow = false; }
        if (s_step_acc < -32) { s_step_acc = 0; if (s_sel < g_chat->n - 1) s_sel++; s_follow = false; }
        s_pad_drag = true;
        return;
    } else if (g_touch.held && g_touch.drag && in_pad) {
        float dy = (float)(g_touch.y - g_touch.py) * 1.7f;
        s_scroll_target -= dy;
        s_scroll = s_scroll_target;
        float inst = g_dt > 0 ? -dy / g_dt : 0;
        s_vel = s_vel * 0.6f + inst * 0.4f;
        s_pad_drag = true;
        moved = true;
    } else if (s_pad_drag && !g_touch.held) {
        s_pad_drag = false;
        s_step_acc = 0;
        if (fabsf(s_vel) < 120) s_vel = 0;
        s_need_snap = true;
    }
    if (!s_pad_drag && s_vel != 0) {
        s_scroll_target += s_vel * g_dt;
        s_scroll = s_scroll_target;
        s_vel *= expf(-g_dt * 5.5f);
        if (fabsf(s_vel) < 60 || s_scroll_target <= 0 || s_scroll_target >= max_scroll()) { s_vel = 0; s_need_snap = true; }
        moved = true;
    }
    circlePosition cp;
    hidCircleRead(&cp);
    float dy = 0;
    if (abs(cp.dy) > 18) dy -= cp.dy * 3.2f * g_dt;
    if (g_kheld & KEY_ZL) dy -= 320 * g_dt;
    if (g_kheld & KEY_ZR) dy += 320 * g_dt;
    bool free_scroll = dy != 0;
    if (free_scroll) { s_scroll_target += dy; s_scroll = s_scroll_target; moved = true; s_vel = 0; }

    // ↑↓ y L/R: de mensaje en mensaje (en mensajes largos, ↑↓ avanza dentro del mensaje)
    if (g_chat->n > 0 && (g_krepeat & (KEY_DUP | KEY_L))) {
        Msg *m = g_chat->m[s_sel];
        float top = m->y - TOP_HDR - 8;
        if ((g_krepeat & KEY_DUP) && s_scroll_target > top + 4) s_scroll_target = fmaxf(top, s_scroll_target - 140);
        else select_msg(s_sel - 1);
        s_follow = false; s_vel = 0;
    }
    if (g_chat->n > 0 && (g_krepeat & (KEY_DDOWN | KEY_R))) {
        Msg *m = g_chat->m[s_sel];
        float view = SCR_H - TOP_HDR - 10;
        float bottom_needed = m->y + m->h - TOP_HDR - view + 4;
        if ((g_krepeat & KEY_DDOWN) && s_scroll_target + 4 < bottom_needed && s_scroll_target + 4 < max_scroll())
            s_scroll_target = fminf(s_scroll_target + 140, bottom_needed);
        else if (s_sel < g_chat->n - 1) select_msg(s_sel + 1);
        else scroll_to_bottom();
        s_vel = 0;
        if (s_sel < g_chat->n - 1) s_follow = false;
    }

    if (moved) {
        s_follow = false;
        s_idle = 0;
        if (s_scroll_target < 0) s_scroll_target = 0;
        if (s_scroll_target > max_scroll()) s_scroll_target = max_scroll();
        int a = msg_at_anchor(s_scroll_target);
        if (a >= 0) s_sel = a;
        if (s_scroll_target >= max_scroll() - 2 && gem_streaming()) s_follow = true;
    } else {
        float prev = s_idle;
        s_idle += g_dt;
        if (prev < 0.25f && s_idle >= 0.25f && free_scroll == false && abs(cp.dy) <= 18) {
            // al soltar el Circle Pad / ZL ZR también se ancla
        }
    }
    // anclar al mensaje resaltado cuando se suelta (si cabe en pantalla)
    if (s_need_snap && !s_pad_drag && s_vel == 0 && g_chat->n > 0 && s_sel >= 0) {
        s_need_snap = false;
        Msg *m = g_chat->m[s_sel];
        float view = SCR_H - TOP_HDR - 16;
        if (m->h <= view && s_scroll_target < max_scroll() - 2) scroll_to_msg(s_sel);
    }
}

void chat_update(void) {
    gem_title_poll();
    pair_poll();              // mensajes compartidos por QR
    mic_update();
    if (S.open && S.closing) sheet_update(&S);

    // fin de una respuesta
    if (gem_poll()) {
        g_chat->updated = osGetTime();
        if (!g_chat->temporary) chat_save(g_chat);
        if (!g_chat->titled && !g_chat->temporary) gem_title_request(g_chat);
        if (S.open && S.kind == SK_DRAWER && !S.closing) { float sc = S.scroll; int sel = S.sel; open_drawer(); S.anim = 1; S.scroll = S.scroll_target = sc; S.sel = sel; }
    }
    int me = gem_take_memory_event();
    if (me > 0) toast(_("Guardado en tu información"));
    else if (me < 0) toast(_("Gemini lo ha olvidado"));

    if (g_dialog.open) {
        int r = dialog_update();
        if (r >= 0) handle_dialog(g_dialog.ctx, r == 1);
        return;
    }

    if (s_recording) {
        if ((g_kdown & (KEY_A | KEY_Y)) || tap(110, 70, 100, 100) || !mic_active()) finish_mic(true);
        else if ((g_kdown & KEY_B) || tap(100, 196, 120, 32)) finish_mic(false);
        return;
    }

    if (s_ov == OV_CAMERA) {
        cam_update(&s_cam_tex);
        if ((g_kdown & KEY_A) || tap(120, 80, 80, 80)) {
            char path[160];
            if (cam_snap(path, sizeof(path))) { cam_close(); s_ov = OV_NONE; attach_image(path); compose(NULL); }
            else toast(_("Espera a que la cámara esté lista"));
        } else if ((g_kdown & KEY_X) || tap(240, 96, 56, 48)) {
            cam_open(!cam_front());
        } else if ((g_kdown & KEY_B) || tap(24, 96, 56, 48)) { cam_close(); s_ov = OV_NONE; }
        return;
    }

    if ((s_ov == OV_QR || s_ov == OV_IMAGE || s_ov == OV_INFO) && (!S.open || S.closing)) {
        if ((g_kdown & (KEY_B | KEY_A)) || g_touch.up) s_ov = OV_NONE;
        return;
    }

    if (S.open && S.kind == SK_USAGE && !S.closing && !g_touch.held && (s_usage_refresh += g_dt) > 1.0f) open_usage(S.sel);
    if (S.open) {
        int r = sheet_update(&S);
        if (r >= 0) handle_sheet_choice(r);
        else if (r == -3 && S.held_item >= 0) handle_sheet_hold(S.held_item);
        else if (r == -4 && S.held_item >= 0) handle_sheet_swipe(S.held_item, S.swipe_action);
        update_overlay_from_sheet();
        if (!S.open && s_ov == OV_PHOTO) { s_ov = OV_NONE; tex_free(&s_ov_tex); }
        return;
    }

    // ---- controles principales
    bool home = g_chat->n == 0;
    if (g_kdown & KEY_START) { dialog_open(DLG_EXIT, _("¿Salir de " APP_NAME "?"), _("Tus chats se guardan automáticamente."), _("Salir"), false); return; }
    if ((g_kdown & KEY_SELECT) || tap(2, 2, 34, 34)) { open_drawer(); return; }
    if (tap(84, 4, 152, 30)) { open_models(); return; }
    float clock_x = 301 - 34 * ease_in_out(s_bar_anim);
    if (tap(clock_x - 17, 2, 34, 34)) {
        if (g_chat->n == 0) { g_chat->temporary = !g_chat->temporary; toast(g_chat->temporary ? _("Chat temporal activado") : _("Chat temporal desactivado")); }
        else { new_chat(true); toast(_("Chat temporal: no se guardará")); }
        return;
    }
    if (!home && s_bar_anim > 0.6f && tap(284, 2, 34, 34)) { new_chat(false); return; }

    // barra de entrada
    if (tap(10, 186, 42, 42)) { open_tools(); return; }
    if (tap(268, 186, 44, 42) || (g_kdown & KEY_Y)) {
        if (gem_streaming()) gem_stop(); else start_mic();
        return;
    }
    if (s_att && tap(52, 186, 40, 42)) { clear_attachment(); return; }
    if (tap(52, 184, 216, 46) || (g_kdown & KEY_A)) { compose(NULL); return; }

    if (home) {
        static const char *init[4] = {"Ayúdame a escribir ", "Explícame de forma sencilla ", "Planifica un viaje de 3 días a ", "Dame ideas para "};
        for (int i = 0; i < 4; i++) {
            float x = 10 + (i % 2) * 154, y = 62 + (i / 2) * 54;
            if (tap(x, y, 146, 46)) { compose(_(init[i])); return; }
        }
        return;
    }

    // conversación
    if (s_sel < 0 || s_sel >= g_chat->n) s_sel = g_chat->n - 1;
    if (gem_streaming()) {
        if (tap(100, ROW_Y + 1, 120, 32) || (g_kdown & KEY_B)) { gem_stop(); return; }
    } else {
        Msg *m = g_chat->m[s_sel];
        bool user = m->role == ROLE_USER;
        if (tap(PAD_X, ROW_Y, 148, 32)) { if (user) edit_message(s_sel); else regenerate(s_sel); return; }
        if (tap(PAD_X + 156, ROW_Y, 148, 32) || (g_kdown & KEY_X)) { open_actions(s_sel); return; }
        if (g_kdown & KEY_B) scroll_to_bottom();
    }
    // el touchpad (y su vista previa) solo desplaza: el menú se abre con el botón «Más»
    update_scroll_input();
}

// ------------------------------------------------------------ maquetación de la conversación
static float user_bubble_w(Msg *m) {
    float w = m->lay.maxw + 20;
    if (w < 40) w = 40;
    if (w > BUB_MAXW) w = BUB_MAXW;
    return w;
}

static float img_box_h(Msg *m, float maxw, float maxh, float *outw) {
    float w = maxw, h = maxh;
    if (m->thumb.ok) {
        float s = fminf(maxw / m->thumb.w, maxh / m->thumb.h);
        if (s > 1.5f) s = 1.5f;
        w = m->thumb.w * s; h = m->thumb.h * s;
    }
    if (outw) *outw = w;
    return h;
}

static void layout_chat(void) {
    float sc = ts();
    float y = TOP_HDR + 8;
    for (int i = 0; i < g_chat->n; i++) {
        Msg *m = g_chat->m[i];
        float h = 0;
        if (m->role == ROLE_USER) {
            if (m->lay_ver != m->ver || m->lay_scale != sc || !m->lay.arena) {
                md_layout(&m->lay, m->text ? m->text : "", BUB_MAXW - 20, sc, false);
                m->lay_ver = m->ver; m->lay_scale = sc;
            }
            if (m->att == ATT_IMAGE) h += img_box_h(m, 120, 90, NULL) + 6;
            if (m->att == ATT_AUDIO) h += 26;
            if (m->tlen) h += m->lay.height + 12;
            else h += 2;
        } else {
            if (m->lay_ver != m->ver || m->lay_scale != sc || !m->lay.arena) {
                md_layout(&m->lay, m->text ? m->text : "", MSG_W, sc, true);
                if (m->thlen) md_layout(&m->tlay, m->thought, MSG_W - 14, sc * 0.92f, true);
                m->lay_ver = m->ver; m->lay_scale = sc;
            }
            h = 4;
            bool streaming_empty = m->state == MST_STREAM && m->tlen == 0;
            if (m->thlen || streaming_empty) h += 22;
            if (m->thlen && m->show_thought) h += m->tlay.height + 10;
            if (streaming_empty) h += 34;
            if (m->tlen) h += m->lay.height;
            if (h < 24) h = 24;
        }
        m->y = y; m->h = h;
        y += h + 14;
    }
    s_content_h = y + 6;
}

static const char *thought_heading(Msg *m, char *out, int n) {
    out[0] = 0;
    if (!m->thought) return out;
    const char *p = m->thought, *last = NULL;
    while ((p = strstr(p, "**"))) {
        const char *e = strstr(p + 2, "**");
        if (!e) break;
        if (e - p > 3 && e - p < 90) last = p;
        p = e + 2;
    }
    if (last) {
        const char *e = strstr(last + 2, "**");
        int l = (int)(e - last - 2);
        if (l > n - 1) l = n - 1;
        memcpy(out, last + 2, l); out[l] = 0;
    }
    return out;
}

static void draw_home_top(void) {
    float t = g_time;
    draw_grad4(0, 120, TOP_W, 120, col_alpha(gemini_grad(t * 0.05f), 0), col_alpha(gemini_grad(t * 0.05f + 0.33f), 0),
               col_alpha(gemini_grad(t * 0.05f), 0.2f), col_alpha(gemini_grad(t * 0.05f + 0.33f), 0.2f));
    if (g_chat->temporary) {
        draw_icon(IC_CLOCK, 200, 80, 40, T.text2);
        text_draw_center(_("Chat temporal"), 200, 112, 0.85f, T.text);
        text_wrap_lines(_("Este chat no aparecerá en tu historial ni se guardará en la tarjeta SD."), 300, 0.5f, 2, 52, 146, T.text2, true);
        return;
    }
    float pulse = 1 + 0.05f * sinf(t * 2);
    draw_circle(200, 72, 30 * pulse, col_alpha(gemini_grad(t * 0.1f), 0.08f));
    draw_sparkle(200, 72, 22 * pulse, t * 0.6f, 1);
    char hi[96];
    if (g_cfg.name[0]) snprintf(hi, sizeof(hi), _("Hola, %s"), g_cfg.name);
    else snprintf(hi, sizeof(hi), _("Hola"));
    float w = text_width(hi, 1.05f);
    if (w > 380) w = 380;
    text_draw_grad(hi, 200 - w / 2, 104, 1.05f, t * 0.08f, 1);
    text_draw_center(_("¿En qué puedo ayudarte hoy?"), 200, 142, 0.66f, T.text2);
}

static void draw_msg(Msg *m, int idx, float sy) {
    float a = m->appear;
    sy += (1 - ease_out_cubic(a)) * 10;
    // resaltado sutil del mensaje seleccionado
    if (idx == s_sel) {
        float g = 0.045f + 0.05f * s_sel_glow;
        draw_rrect(4, sy - 5, TOP_W - 10, m->h + 10, 10, col_alpha(T.accent, g * a));
        draw_rrect(4, sy - 3, 2.5f, m->h + 6, 1.2f, col_alpha(T.accent, (0.5f + 0.35f * s_sel_glow) * a));
    }
    if (m->role == ROLE_USER) {
        float bw = user_bubble_w(m);
        float bx = TOP_W - 12 - bw;
        float y = sy;
        if (m->att == ATT_IMAGE) {
            float iw; float ih = img_box_h(m, 120, 90, &iw);
            if (!m->thumb.ok) draw_rrect(TOP_W - 12 - iw, y, iw, ih, 10, col_alpha(T.surface2, a));
            else draw_texture(&m->thumb, TOP_W - 12 - iw, y, iw, ih, a);
            y += ih + 6;
        }
        if (m->att == ATT_AUDIO) {
            draw_rrect(TOP_W - 12 - 130, y, 130, 22, 11, col_alpha(T.user_bubble, a));
            draw_icon(IC_MIC, TOP_W - 12 - 116, y + 11, 14, col_alpha(T.accent, a));
            for (int k = 0; k < 14; k++) {
                float hh = 3 + fabsf(sinf(k * 1.7f + idx)) * 9;
                draw_rect(TOP_W - 12 - 100 + k * 6, y + 11 - hh / 2, 3, hh, col_alpha(T.text2, a));
            }
            y += 26;
        }
        if (m->tlen) {
            draw_rrect(bx, y, bw, m->lay.height + 10, 14, col_alpha(T.user_bubble, a));
            md_draw(&m->lay, bx + 10, y + 5, 0, SCR_H, T.user_text, a);
        }
        return;
    }
    bool streaming = m->state == MST_STREAM;
    if (m->state == MST_ERROR) draw_icon(IC_INFO, 17, sy + 10, 16, col_alpha(T.danger, a));
    else draw_sparkle(17, sy + 10, streaming ? 8 + sinf(g_time * 5) * 1.2f : 8, streaming ? g_time * 3.5f : 0, a);
    float y = sy + 4;
    bool streaming_empty = streaming && m->tlen == 0;
    if (m->thlen || streaming_empty) {
        char head[96], buf[160];
        thought_heading(m, head, sizeof(head));
        if (streaming && m->tlen == 0) {
            const char *st = gem_status();
            if (st[0]) snprintf(buf, sizeof(buf), "%s", st);
            else snprintf(buf, sizeof(buf), "%s", head[0] ? head : (g_cfg.mode == MODE_FAST ? _("Generando…") : _("Pensando…")));
            text_draw_grad(buf, MSG_X, y, 0.48f, g_time * 0.6f, a);
        } else {
            snprintf(buf, sizeof(buf), "%s", m->show_thought ? _("Ocultar razonamiento") : _("Mostrar razonamiento"));
            float w = text_width(buf, 0.46f);
            draw_rrect(MSG_X - 4, y - 1, w + 26, 18, 9, col_alpha(T.surface, a));
            text_draw(buf, MSG_X + 2, y, 0.46f, col_alpha(T.text2, a));
            draw_icon(m->show_thought ? IC_UP : IC_DOWN, MSG_X + w + 12, y + 8, 10, col_alpha(T.text2, a));
        }
        y += 22;
        if (m->thlen && m->show_thought) {
            draw_rect(MSG_X, y, 2, m->tlay.height, col_alpha(T.divider, a));
            md_draw(&m->tlay, MSG_X + 10, y, 0, SCR_H, T.text3, a);
            y += m->tlay.height + 10;
        }
    }
    if (streaming_empty) {
        float ph = g_time * 0.8f;
        draw_shimmer_bar(MSG_X, y + 2, MSG_W, 8, ph);
        draw_shimmer_bar(MSG_X, y + 14, MSG_W * 0.85f, 8, ph - 0.1f);
        draw_shimmer_bar(MSG_X, y + 26, MSG_W * 0.6f, 8, ph - 0.2f);
        y += 34;
    }
    if (m->tlen) {
        md_draw(&m->lay, MSG_X, y, 0, SCR_H, m->state == MST_ERROR ? T.danger : T.text, a);
        if (streaming && m->lay.nline > 0) {
            MdLine *l = &m->lay.lines[m->lay.nline - 1];
            float cx = MSG_X;
            if (l->nseg) { MdSeg *s = &m->lay.segs[l->seg0 + l->nseg - 1]; cx += s->x + s->w + 2; }
            float blink = 0.5f + 0.5f * sinf(g_time * 8);
            draw_circle(cx + 3, y + l->y + l->h / 2, 3, col_alpha(gemini_grad(g_time * 0.3f), blink));
        }
    }
}

static void draw_header_top(void) {
    draw_grad_v(0, 0, TOP_W, TOP_HDR + 10, T.bg, col_alpha(T.bg, 0));
    draw_rect(0, 0, TOP_W, TOP_HDR - 4, T.bg);
    text_draw("Gemini", 10, 3, 0.55f, T.text);
    text_draw_clip(gem_mode_label(g_cfg.mode), 10 + text_width("Gemini", 0.55f) + 6, 5, 200, 0.45f, T.text3);
    float rx = TOP_W - 10;
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    char clk[8]; snprintf(clk, sizeof(clk), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    text_draw_right(clk, rx, 5, 0.45f, T.text3);
    rx -= text_width(clk, 0.45f) + 8;
    if (g_chat->temporary) draw_icon(IC_CLOCK, rx - 7, 11, 13, T.accent);
    if (gem_streaming()) {
        float p = g_time * 1.2f;
        draw_grad_h(0, TOP_HDR - 4, TOP_W, 2, col_alpha(gemini_grad(p), 0.9f), col_alpha(gemini_grad(p + 0.5f), 0.9f));
    }
}

static void draw_overlay_top(void) {
    s_ov_anim = approach(s_ov_anim, 1, 12);
    float a = ease_out_cubic(s_ov_anim);
    switch (s_ov) {
    case OV_QR: {
        draw_rect(0, 0, TOP_W, SCR_H, col_alpha(T.bg, a));
        draw_qr(s_ov_url[0] ? s_ov_url : s_ov_text, 12, 8, 224, a);
        float tx = 248;
        text_wrap_lines(s_ov_title, 140, 0.6f, 2, tx, 16, col_alpha(T.text, a), true);
        int nl = text_wrap_lines(s_ov_cap, 140, 0.45f, 5, tx, 58, col_alpha(T.text2, a), true);
        float py = 58 + nl * text_lineh(0.45f) + 10;
        if (s_ov_url[0]) {
            text_wrap_lines(s_ov_url + 7, 140, 0.42f, 2, tx, py, col_alpha(T.accent, a), true);
            py += 2 * text_lineh(0.42f) + 6;
        }
        char pv[300]; snprintf(pv, sizeof(pv), "%.180s", s_ov_text);
        int maxl = (int)((210 - py) / text_lineh(0.4f));
        if (maxl > 0) text_wrap_lines(pv, 140, 0.4f, maxl, tx, py, col_alpha(T.text3, a), true);
        text_draw(_(GLYPH_B " Cerrar"), tx, 218, 0.45f, col_alpha(T.text2, a));
        break;
    }
    case OV_INFO: {
        draw_rect(0, 0, TOP_W, SCR_H, col_alpha(T.bg, a));
        bool qr = s_ov_text[0] != 0;
        float tx = qr ? 176 : 24, tw = qr ? 212 : 352;
        if (qr) {
            draw_qr(s_ov_text, 12, 30, 152, a);
            text_draw_center(s_ov_text + 8, 88, 190, 0.42f, col_alpha(T.accent, a));
        }
        draw_sparkle(tx + 9, 24, 9, g_time * 0.5f, a);
        text_draw(s_ov_title, tx + 24, 14, 0.65f, col_alpha(T.text, a));
        float sc = 0.44f;          // se reduce un poco si el texto no cabe entero
        while (sc > 0.36f && text_wrap_lines(s_ov_cap, tw, sc, 40, 0, 0, 0, false) * text_lineh(sc) > SCR_H - 52) sc -= 0.02f;
        text_wrap_lines(s_ov_cap, tw, sc, 40, tx, 44, col_alpha(T.text2, a), true);
        break;
    }
    case OV_IMAGE: case OV_PHOTO: {
        draw_rect(0, 0, TOP_W, SCR_H, col_alpha(0xFF000000, a));
        if (s_ov_tex.ok) {
            float s = fminf(400.0f / s_ov_tex.w, 240.0f / s_ov_tex.h);
            float w = s_ov_tex.w * s * (0.94f + 0.06f * a), h = s_ov_tex.h * s * (0.94f + 0.06f * a);
            draw_texture(&s_ov_tex, (TOP_W - w) / 2, (SCR_H - h) / 2, w, h, a);
        }
        draw_grad_v(0, 200, TOP_W, 40, 0, col_alpha(0xFF000000, 0.8f * a));
        text_draw_clip(s_ov_cap, 10, 220, 380, 0.42f, col_alpha(0xFFE3E3E3, a));
        break;
    }
    case OV_CAMERA:
        draw_rect(0, 0, TOP_W, SCR_H, 0xFF000000);
        if (s_cam_tex.ok) draw_texture(&s_cam_tex, 0, 0, 400, 240, 1);
        else text_draw_center(_("Iniciando cámara…"), 200, 110, 0.6f, 0xFFE3E3E3);
        draw_grad_v(0, 0, TOP_W, 26, col_alpha(0xFF000000, 0.6f), 0);
        text_draw(cam_front() ? _("Cámara interior") : _("Cámara exterior"), 10, 5, 0.45f, 0xFFE3E3E3);
        break;
    }
}

static void stat_box(float x, float y, float w, const char *label, const char *val, const char *sub, u32 vc, float a) {
    draw_rrect(x, y, w, 58, 12, col_alpha(T.surface, a));
    text_draw_clip(label, x + 10, y + 6, w - 20, 0.42f, col_alpha(T.text3, a));
    text_draw_clip(val, x + 10, y + 20, w - 20, 0.72f, col_alpha(vc, a));
    if (sub && sub[0]) text_draw_clip(sub, x + 10, y + 42, w - 20, 0.38f, col_alpha(T.text3, a));
}

// detalles del modelo resaltado en «Uso de los modelos»
static void draw_usage_top(SheetItem *it, float a) {
    draw_rect(0, 0, TOP_W, SCR_H, col_alpha(T.bg, a));
    int hh, mm;
    char reset[120];
    if (usage_reset_local(&hh, &mm)) snprintf(reset, sizeof(reset), _("El cupo gratuito se reinicia a las %02d:%02d (medianoche en California)."), hh, mm);
    else snprintf(reset, sizeof(reset), _("El cupo gratuito se reinicia a medianoche, hora de California."));
    if (it && it->id == -2) {
        draw_qr(URL_USAGE, 12, 8, 224, a);
        float tx = 248;
        text_wrap_lines(_("Uso oficial"), 140, 0.62f, 2, tx, 16, col_alpha(T.text, a), true);
        text_wrap_lines(_("Google muestra en AI Studio el uso exacto y los límites de tu clave. Escanéalo con tu teléfono."), 140, 0.45f, 7, tx, 46, col_alpha(T.text2, a), true);
        text_wrap_lines(URL_USAGE + 8, 140, 0.4f, 2, tx, 200, col_alpha(T.accent, a), true);
        return;
    }
    UsageModel *u = (it && it->id >= 0) ? usage_get(it->id) : NULL;
    if (!u) {
        draw_icon(IC_BOLT, 22, 22, 18, col_alpha(T.accent, a));
        text_draw(_("Uso de los modelos"), 40, 13, 0.62f, col_alpha(T.text, a));
        int r, t; usage_totals(&r, &t);
        char v[24], tk[16];
        snprintf(v, sizeof(v), "%d", r); fmt_tokens(t, tk, sizeof(tk));
        stat_box(14, 46, 182, _("Peticiones hoy"), v, NULL, T.text, a);
        stat_box(204, 46, 182, _("Tokens hoy"), tk, NULL, T.text, a);
        text_wrap_lines(_("Google no informa de cuánto cupo te queda, así que Gemi3DS cuenta aquí cada petición. "
                          "Si Google rechaza una por límite, se apunta el límite que indica."), 360, 0.45f, 4, 20, 120, col_alpha(T.text2, a), true);
        text_wrap_lines(reset, 360, 0.43f, 2, 20, 196, col_alpha(T.text3, a), true);
        return;
    }
    u32 sc; const char *st = usage_state(u, &sc);
    draw_sparkle(22, 22, 9, g_time * 0.5f, a);
    text_draw_clip(model_name(u->id), 40, 11, 270, 0.62f, col_alpha(T.text, a));
    float sw = text_width(st, 0.45f) + 20;
    draw_rrect(386 - sw, 12, sw, 20, 10, col_alpha(col_lerp(T.surface, sc, 0.25f), a));
    text_draw_center(st, 386 - sw / 2, 15, 0.45f, col_alpha(sc, a));
    text_draw_clip(u->id, 40, 32, 340, 0.38f, col_alpha(T.text3, a));

    char v[32], sub[64], tk[16], ti[16], to[16];
    // peticiones hoy (con barra si se conoce el límite)
    if (u->limit_day) { snprintf(v, sizeof(v), "%d / %d", u->req, u->limit_day); snprintf(sub, sizeof(sub), _("Límite diario según Google")); }
    else { snprintf(v, sizeof(v), "%d", u->req); snprintf(sub, sizeof(sub), _("%d correctas"), u->ok); }
    stat_box(14, 50, 182, _("Peticiones hoy"), v, sub, T.text, a);
    if (u->limit_day) {            // barra junto al número: cuánto del límite diario se ha usado
        float f = clampf((float)u->req / u->limit_day, 0, 1);
        float bx = 24 + text_width(v, 0.72f) + 12, bw = 186 - bx;
        if (bw > 20) {
            draw_rrect(bx, 79, bw, 4, 2, col_alpha(T.divider, a));
            draw_rrect(bx, 79, fmaxf(4, bw * f), 4, 2, col_alpha(f >= 0.9f ? T.danger : T.accent, a));
        }
    }
    int rpm = usage_rpm(u);
    if (u->limit_min) snprintf(v, sizeof(v), "%d / %d", rpm, u->limit_min); else snprintf(v, sizeof(v), "%d", rpm);
    stat_box(204, 50, 182, _("Último minuto"), v, u->limit_min ? _("Límite por minuto según Google") : _("Peticiones en 60 s"), T.text, a);
    fmt_tokens(u->tok_in + u->tok_out, tk, sizeof(tk)); fmt_tokens(u->tok_in, ti, sizeof(ti)); fmt_tokens(u->tok_out, to, sizeof(to));
    snprintf(sub, sizeof(sub), _("%s entrada · %s salida"), ti, to);
    stat_box(14, 116, 182, _("Tokens hoy"), tk, sub, T.text, a);
    if (u->last_time) {
        int ago = (int)((osGetTime() - u->last_time) / 1000);
        if (ago < 60) snprintf(sub, sizeof(sub), _("hace %d s"), ago);
        else if (ago < 3600) snprintf(sub, sizeof(sub), _("hace %d min"), ago / 60);
        else snprintf(sub, sizeof(sub), _("hace %d h"), ago / 3600);
        if (u->last_code == 200 && u->last_ms > 0) snprintf(v, sizeof(v), "%.1f s", u->last_ms / 1000.0f);
        else snprintf(v, sizeof(v), u->last_code == 200 ? "OK" : "Error %d", u->last_code);
        stat_box(204, 116, 182, _("Última respuesta"), v, sub, u->last_code == 200 ? T.text : T.danger, a);
    } else {
        stat_box(204, 116, 182, _("Última respuesta"), "—", _("Sin peticiones en esta sesión"), T.text3, a);
    }
    int ps = gem_model_paused(u->id);
    char note[200];
    if (ps) snprintf(note, sizeof(note), _("En pausa %d min: iba lento o saturado; los modos automáticos usan otro mientras tanto."), (ps + 59) / 60);
    else snprintf(note, sizeof(note), "%s", reset);
    text_wrap_lines(note, 364, 0.43f, 3, 18, 184, col_alpha(T.text3, a), true);
}

// información contextual en la pantalla superior mientras hay una hoja abierta
static void draw_sheet_context_top(void) {
    if (!S.open) return;
    SheetItem *it = sheet_selected(&S);
    float a = ease_out_cubic(S.anim);
    if (S.kind == SK_MODELS && it) {
        draw_rect(0, 150, TOP_W, 90, col_alpha(T.bg, 0.92f * a));
        draw_rect(0, 150, TOP_W, 1, col_alpha(T.divider, a));
        const char *desc = "";
        if (it->id == MODE_FAST) desc = _("Ideal para el día a día. Usa el modelo Flash más rápido disponible y, si uno se satura o va lento, cambia solo a otro gratuito.");
        else if (it->id == MODE_THINK) desc = _("Gemini piensa antes de responder. Mejor para problemas complejos, matemáticas y planificación. Puedes ver su razonamiento.");
        else if (it->id == MODE_PRO) desc = _("El modelo más capaz para tareas difíciles. Normalmente requiere activar la facturación en Google AI Studio.");
        else if (it->id >= 100 && it->id - 100 < g_models.n) desc = g_models.list[it->id - 100].desc;
        text_draw(it->label, 14, 158, 0.55f, col_alpha(T.text, a));
        text_wrap_lines(desc, 372, 0.44f, 4, 14, 180, col_alpha(T.text2, a), true);
    } else if (S.kind == SK_TOOLS && it) {
        draw_rect(0, 196, TOP_W, 44, col_alpha(T.bg, 0.92f * a));
        text_draw_clip(it->sub, 14, 210, 372, 0.46f, col_alpha(T.text2, a));
    } else if ((S.kind == SK_MEMORY || S.kind == SK_MEMOPT) && it) {
        draw_rect(0, 0, TOP_W, SCR_H, col_alpha(T.bg, a));
        draw_icon(IC_NOTE, 22, 22, 18, col_alpha(T.accent, a));
        text_draw(_("Información guardada"), 40, 13, 0.62f, col_alpha(T.text, a));
        int mi = S.kind == SK_MEMOPT ? S.ctx : it->id;
        if (mi >= 0) {
            char line[600]; mem_get(mi, line, sizeof(line));
            draw_rrect(14, 44, 372, 124, 12, col_alpha(T.surface, a));
            text_wrap_lines(line, 350, 0.52f, 7, 26, 54, col_alpha(T.text, a), true);
        } else {
            text_wrap_lines(_("Gemini usa esta información en todas tus conversaciones para darte respuestas más personales. "
                            "Puedes añadir datos aquí o decirle en el chat \"recuerda que…\". Para que olvide algo, "
                            "elimínalo de la lista o dile \"olvida que…\"."), 360, 0.47f, 7, 20, 50, col_alpha(T.text2, a), true);
        }
        char n[60]; snprintf(n, sizeof(n), "%d %s", mem_count(), mem_count() == 1 ? _("dato guardado") : _("datos guardados"));
        text_draw(n, 20, 212, 0.45f, col_alpha(T.text3, a));
    } else if (S.kind == SK_USAGE) {
        draw_usage_top(it, a);
    } else if (S.kind == SK_ABOUT) {
        draw_rect(0, 0, TOP_W, SCR_H, col_alpha(T.bg, a));
        const char *url = NULL, *name = NULL, *desc = NULL;
        if (it && it->id == 1) { url = URL_GITHUB; name = "GitHub"; desc = _("Código, novedades y nuevas versiones de mis proyectos."); }
        if (it && it->id == 2) { url = URL_DISCORD; name = "Discord"; desc = _("Únete a la comunidad: dudas, ideas y avisos de actualizaciones."); }
        if (it && it->id == 3) { url = URL_KOFI; name = "Ko-fi"; desc = _("Si te gusta Gemi3DS, invítame a un café, ¡ayuda muchísimo!"); }
        if (url) {
            draw_qr(url, 12, 8, 224, a);
            float tx = 248;
            text_draw(name, tx, 16, 0.75f, col_alpha(T.text, a));
            text_wrap_lines(desc, 140, 0.45f, 5, tx, 48, col_alpha(T.text2, a), true);
            text_draw(_("Escanéalo con"), tx, 150, 0.45f, col_alpha(T.text3, a));
            text_draw(_("tu teléfono"), tx, 166, 0.45f, col_alpha(T.text3, a));
            text_wrap_lines(url + 8, 140, 0.4f, 3, tx, 192, col_alpha(T.accent, a), true);
        } else {
            draw_sparkle(40, 40, 22, g_time * 0.5f, a);
            text_draw_grad(APP_NAME, 74, 20, 1.0f, g_time * 0.1f, a);
            char vb[80]; snprintf(vb, sizeof(vb), _("v%s · hecho por %s"), APP_VERSION, APP_AUTHOR);
            text_draw(vb, 76, 52, 0.47f, col_alpha(T.text2, a));
            text_wrap_lines(_(ABOUT_TEXT), 364, 0.44f, 12, 18, 80, col_alpha(T.text2, a), true);
        }
    }
}

void chat_draw_top(void) {
    LightLock_Lock(&g_chat_lock);
    layout_chat();
    float maxs = max_scroll();
    if (s_follow) {
        s_scroll_target = maxs;
        if (g_chat->n) s_sel = g_chat->n - 1;
    }
    if (s_scroll_target > maxs) s_scroll_target = maxs;
    if (s_scroll_target < 0) s_scroll_target = 0;
    if (s_scroll > maxs + 1) s_scroll = maxs;
    s_scroll = approach(s_scroll, s_scroll_target, 14);
    if (g_chat->n && (s_sel < 0 || s_sel >= g_chat->n)) s_sel = g_chat->n - 1;
    if (s_sel != s_last_sel) { s_sel_glow = 1; s_last_sel = s_sel; }
    s_sel_glow = approach(s_sel_glow, 0, 4);

    if (g_chat->n == 0) draw_home_top();
    for (int i = 0; i < g_chat->n; i++) {
        Msg *m = g_chat->m[i];
        float sy = m->y - s_scroll;
        bool vis = sy + m->h > -40 && sy < SCR_H + 40;
        if (!vis) {
            if ((sy + m->h < -700 || sy > SCR_H + 700) && m->thumb.ok) { tex_free(&m->thumb); m->thumb_tried = false; }
            continue;
        }
        if (m->att == ATT_IMAGE && !m->thumb.ok && !m->thumb_tried) {
            m->thumb_tried = true;
            img_load_tex(m->att_path, &m->thumb, 180, 135);
        }
        m->appear = approach(m->appear, 1, 9);
        draw_msg(m, i, sy);
    }
    if (s_content_h > SCR_H) {
        float view = SCR_H - TOP_HDR;
        float bh = fmaxf(16, view * SCR_H / s_content_h);
        float by = TOP_HDR + (view - bh) * (s_scroll / fmaxf(1, s_content_h - SCR_H));
        draw_rrect(TOP_W - 4, by, 2.5f, bh, 1.2f, col_alpha(T.text3, 0.45f));
    }
    LightLock_Unlock(&g_chat_lock);
    draw_header_top();
    draw_sheet_context_top();
    if (s_ov != OV_NONE) draw_overlay_top();
}

// ------------------------------------------------------------ pantalla inferior
static void draw_top_bar(void) {
    ui_icon_button(IC_MENU, 19, 19, 15, T.text2, 0);
    char lbl[80]; snprintf(lbl, sizeof(lbl), "%s", gem_mode_label(g_cfg.mode));
    float w = text_width(lbl, 0.5f) + 34;
    if (w > 152) w = 152;
    float x = 160 - w / 2;
    bool p = touch_in(84, 4, 152, 30);
    draw_rrect(x, 7, w, 24, 12, p ? T.pill_hi : T.surface);
    text_draw_clip(lbl, x + 12, 12, w - 32, 0.5f, T.text);
    draw_icon(IC_CHEV_DOWN, x + w - 13, 19, 11, T.text2);
    // en el inicio el reloj ocupa la esquina; al empezar el chat se desliza y aparece el lápiz
    s_bar_anim = approach(s_bar_anim, g_chat->n > 0 ? 1.0f : 0.0f, 9);
    float t = s_bar_anim;
    float cx = 301 - 34 * ease_in_out(t);
    bool temp = g_chat->temporary;
    if (temp) draw_circle(cx, 19, 15, T.accent_soft);
    ui_icon_button(IC_CLOCK, cx, 19, 15, temp ? T.accent : T.text2, 0);
    if (t > 0.02f) {
        float a = clampf((t - 0.25f) / 0.75f, 0, 1);
        float sc = ease_out_back(a);
        if (touch_in(284, 2, 34, 34) && t > 0.6f) draw_circle(301, 19, 15, col_alpha(T.text, 0.12f));
        draw_icon(IC_EDIT, 301, 19 + (1 - a) * 6, 17.25f * sc, col_alpha(T.text2, a));
    }
}

static void draw_input_bar(void) {
    bool p = touch_in(52, 184, 216, 46);
    draw_rrect(8, 184, 304, 46, 23, p ? T.pill_hi : T.pill);
    if (!T.dark) draw_rrect_outline(8, 184, 304, 46, 23, 1, T.divider);
    ui_icon_button(IC_PLUS, 31, 207, 15, T.text2, 0);
    float tx = 54;
    if (s_att) {
        if (s_att == ATT_IMAGE && s_att_tex.ok) draw_texture(&s_att_tex, 54, 194, 30, 26, 1);
        else { draw_rrect(54, 194, 30, 26, 6, T.surface2); draw_icon(s_att == ATT_AUDIO ? IC_MIC : IC_IMAGE, 69, 207, 14, T.accent); }
        draw_circle(84, 194, 6, T.text3);
        draw_icon(IC_CLOSE, 84, 194, 7, T.bg);
        tx = 96;
    }
    const char *ph = s_att == ATT_IMAGE ? _("Pregunta sobre la foto") : _("Pregúntale a Gemini");
    text_draw_clip(ph, tx, 199, 262 - tx, 0.55f, T.text3);
    if (gem_streaming()) {
        draw_circle(289, 207, 16, T.text);
        draw_icon(IC_STOP, 289, 207, 13, T.bg);
    } else {
        ui_icon_button(IC_MIC, 289, 207, 15, T.text2, 0);
    }
}

static void draw_home_bot(void) {
    static const IconId ic[4] = {IC_PEN, IC_BULB, IC_MAP, IC_STAR};
    static const u32 icc[4] = {0x8AB4F8, 0xFDD663, 0x81C995, 0xF28B82};
    static const char *lb[4] = {"Ayúdame a escribir", "Explícame un tema", "Planifica un viaje", "Dame ideas"};
    text_draw_center(_("Prueba con…"), 160, 43, 0.45f, T.text3);
    for (int i = 0; i < 4; i++) {
        float x = 10 + (i % 2) * 154, y = 62 + (i / 2) * 54;
        float app = clampf(g_time * 3 - i * 0.15f, 0, 1);
        bool p = touch_in(x, y, 146, 46);
        draw_rrect(x, y + (1 - ease_out_cubic(app)) * 8, 146, 46, 14, col_alpha(p ? T.pill_hi : T.surface, app));
        draw_icon(ic[i], x + 21, y + 23, 18, col_alpha(col_hex(icc[i]), app));
        text_draw_clip(_(lb[i]), x + 38, y + 15, 104, 0.47f, col_alpha(T.text, app));
    }
}

// touchpad para recorrer la conversación de la pantalla superior
static void draw_touchpad(void) {
    bool active = s_pad_drag || fabsf(s_vel) > 1;
    u32 bg = active ? col_lerp(T.surface, T.accent, 0.08f) : T.surface;
    draw_rrect(PAD_X, PAD_Y, PAD_W, PAD_H, 16, bg);
    // trama de puntos que se desplaza con el chat
    u32 dot = col_alpha(T.text3, active ? 0.35f : 0.16f);
    float off = fmodf(s_scroll * 0.35f, 12.0f);
    if (off < 0) off += 12;
    for (float y = PAD_Y + 30 - off; y < PAD_Y + PAD_H - 6; y += 12) {
        if (y < PAD_Y + 26) continue;
        for (float x = PAD_X + 16; x < PAD_X + PAD_W - 26; x += 12) draw_rect(x, y, 1.6f, 1.6f, dot);
    }
    // cabecera: mensaje resaltado
    Msg *m = (s_sel >= 0 && s_sel < g_chat->n) ? g_chat->m[s_sel] : NULL;
    if (m) {
        if (m->role == ROLE_USER) draw_icon(IC_USER, PAD_X + 16, PAD_Y + 14, 12, T.text2);
        else draw_sparkle(PAD_X + 16, PAD_Y + 14, 6, 0, 1);
        char who[240], prev[200];
        md_strip(m->text ? m->text : "", prev, sizeof(prev));
        for (char *q = prev; *q; q++) if (*q == '\n') *q = ' ';
        if (!prev[0]) snprintf(prev, sizeof(prev), "%s", m->att == ATT_AUDIO ? _("Nota de voz") : (m->att ? _("Foto") : "…"));
        snprintf(who, sizeof(who), "%s: %s", m->role == ROLE_USER ? _("Tú") : "Gemini", prev);
        text_draw_clip(who, PAD_X + 28, PAD_Y + 7, PAD_W - 96, 0.44f, T.text2);
        char pos[24]; snprintf(pos, sizeof(pos), "%d / %d", s_sel + 1, g_chat->n);
        text_draw_right(pos, PAD_X + PAD_W - 24, PAD_Y + 7, 0.42f, T.accent);
    }
    // indicador de posición (derecha)
    float tx = PAD_X + PAD_W - 12, ty = PAD_Y + 28, th = PAD_H - 38;
    draw_rrect(tx - 1.5f, ty, 3, th, 1.5f, col_alpha(T.text3, 0.25f));
    float frac = max_scroll() > 0 ? s_scroll / max_scroll() : 1;
    float vis = s_content_h > 0 ? fminf(1, SCR_H / s_content_h) : 1;
    float kh = fmaxf(10, th * vis);
    draw_rrect(tx - 2.5f, ty + (th - kh) * clampf(frac, 0, 1), 5, kh, 2.5f, T.accent);
    // estado / guía
    float cy = PAD_Y + 60;
    if (gem_streaming()) {
        Msg *t = gem_target();
        const char *st = gem_status();
        char buf[160];
        if (st[0]) snprintf(buf, sizeof(buf), "%s", st);
        else snprintf(buf, sizeof(buf), "%s · %s", (t && t->tlen == 0 && t->thlen) ? _("Pensando") : _("Respondiendo"), t ? t->model : "");
        float w = text_width(buf, 0.45f);
        if (w > PAD_W - 70) w = PAD_W - 70;
        float x0 = PAD_X + (PAD_W - 12 - (w + 22)) / 2;
        draw_sparkle(x0 + 7, cy, 7, g_time * 3, 1);
        text_draw_clip(buf, x0 + 20, cy - 7, PAD_W - 70, 0.45f, T.text);
    } else if (!active) {
        float a = 0.55f + 0.2f * sinf(g_time * 2);
        float cx = PAD_X + (PAD_W - 12) / 2;
        draw_icon(IC_UP, cx, cy - 15, 10, col_alpha(T.text3, a));
        text_draw_center(_("Desliza para recorrer el chat"), cx, cy - 7, 0.45f, col_alpha(T.text3, a));
        draw_icon(IC_DOWN, cx, cy + 15, 10, col_alpha(T.text3, a));
    }
}

static void draw_chat_bot(void) {
    draw_touchpad();
    if (gem_streaming()) {
        ui_pill_button(_("Detener"), IC_STOP, 100, ROW_Y + 1, 120, 32, T.bg, T.text);
        return;
    }
    if (s_sel < 0 || s_sel >= g_chat->n) return;
    bool user = g_chat->m[s_sel]->role == ROLE_USER;
    ui_pill_button(user ? _("Editar") : _("Otra respuesta"), user ? IC_EDIT : IC_REFRESH, PAD_X, ROW_Y, 148, 32, T.text, T.surface2);
    ui_pill_button(_("Más"), IC_MENU, PAD_X + 156, ROW_Y, 148, 32, T.text, T.surface2);
}

static void draw_recording_bot(void) {
    draw_rect(0, 0, BOT_W, SCR_H, T.bg2);
    float lv = clampf(mic_level() * 4, 0, 1);
    for (int i = 3; i >= 1; i--) {
        float r = 34 + i * 10 * lv + sinf(g_time * 4 + i) * 2;
        draw_circle(160, 120, r, col_alpha(gemini_grad(i / 4.0f + g_time * 0.1f), 0.12f));
    }
    draw_circle(160, 120, 34, T.accent);
    draw_icon(IC_MIC, 160, 120, 34, on_accent());
    char t[40]; snprintf(t, sizeof(t), _("Escuchando… %.0f s"), mic_seconds());
    text_draw_center(t, 160, 26, 0.6f, T.text);
    text_draw_center(_("Toca el micrófono o pulsa " GLYPH_A " para enviar"), 160, 50, 0.45f, T.text3);
    ui_pill_button(_("Cancelar"), IC_CLOSE, 100, 196, 120, 32, T.text, T.surface2);
}

static void draw_camera_bot(void) {
    draw_rect(0, 0, BOT_W, SCR_H, 0xFF101010);
    text_draw_center(_("Encuadra y toca para hacer la foto"), 160, 20, 0.5f, 0xFFE3E3E3);
    bool p = touch_in(120, 80, 80, 80);
    draw_circle(160, 120, 36, 0xFFFFFFFF);
    draw_circle(160, 120, p ? 26 : 30, p ? 0xFFDDDDDD : 0xFFFFFFFF);
    draw_ring(160, 120, 33, 2, 0xFF101010);
    ui_icon_button(IC_CLOSE, 52, 120, 22, 0xFFE3E3E3, 0xFF2A2A2A);
    ui_icon_button(IC_FLIP, 268, 120, 22, 0xFFE3E3E3, 0xFF2A2A2A);
    text_draw_center(_(GLYPH_A " Foto   " GLYPH_X " Cambiar cámara   " GLYPH_B " Cancelar"), 160, 210, 0.42f, 0xFFA0A0A0);
}

void chat_draw_bot(void) {
    if (s_ov == OV_CAMERA) { draw_camera_bot(); return; }
    if (s_recording) { draw_recording_bot(); return; }
    draw_top_bar();
    if (g_chat->n == 0) draw_home_bot();
    else draw_chat_bot();
    draw_input_bar();
    sheet_draw(&S);
    if ((s_ov == OV_QR || s_ov == OV_IMAGE || s_ov == OV_INFO) && !S.open) {
        draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, 0.85f));
        text_draw_center(_("Toca la pantalla o pulsa " GLYPH_B " para cerrar"), 160, 112, 0.5f, 0xFFE3E3E3);
    }
    dialog_draw();
}

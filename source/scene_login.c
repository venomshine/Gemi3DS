/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - pantalla de vinculación (QR + teléfono)
#include "app.h"
#include "ui.h"
#include "pair.h"
#include "gemini.h"
#include "store.h"
#include "scenes.h"
#include "util.h"
#include "net.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

enum { LS_WAIT, LS_CHECK, LS_OK, LS_ERR };
static int s_st;
static float s_t, s_ok_t;
static char s_err[400];
static char s_key[128], s_name[48];
static bool s_help;
static bool s_manual;
static bool s_wait_net;         // la clave está guardada en espera: se comprobará al haber conexión
static float s_net_t;           // para revisar la conexión cada poco
static float s_retry_t;         // tiempo desde el último intento con la clave en espera

void login_enter(void) {
    s_st = LS_WAIT; s_t = 0; s_err[0] = 0; s_help = false; s_wait_net = false;
    pair_start();
}

void login_leave(void) { pair_stop(); }

static void start_check(const char *key, const char *name, bool manual) {
    snprintf(s_key, sizeof(s_key), "%s", key);
    snprintf(s_name, sizeof(s_name), "%s", name ? name : "");
    s_manual = manual;
    s_net_t = 0;
    if (!net_local_ip()) {          // sin Wi-Fi: se guarda en espera en vez de fallar
        s_st = LS_ERR; s_wait_net = true;
        snprintf(s_err, sizeof(s_err), "%s", _("Sin conexión Wi-Fi. Comprobaré la clave en cuanto te conectes."));
        return;
    }
    s_wait_net = false;
    s_st = LS_CHECK;
    pair_set_status(PAIR_CHECKING, "");
    gem_validate_key(s_key);
}

void login_update(void) {
    s_t += g_dt;
    pair_poll();
    // revisar la conexión: el QR aparece solo al conectar el Wi-Fi y la clave en espera se comprueba sola
    s_net_t += g_dt; s_retry_t += g_dt;
    if (s_net_t > 1.5f && s_st != LS_CHECK && s_st != LS_OK) {
        s_net_t = 0;
        bool online = net_local_ip() != 0;
        if (online && strncmp(pair_url(), "http", 4)) { pair_stop(); pair_start(); }
        if (online && s_wait_net && s_st == LS_ERR && s_retry_t > 4) { s_retry_t = 0; start_check(s_key, s_name, s_manual); }
    }
    char k[128], n[48];
    if (s_st != LS_CHECK && s_st != LS_OK && pair_take(k, sizeof(k), n, sizeof(n))) start_check(k, n, false);

    if (s_st == LS_CHECK) {
        char msg[400];
        int r = gem_validate_poll(msg, sizeof(msg));
        if (r == 1) {
            snprintf(g_cfg.api_key, sizeof(g_cfg.api_key), "%s", s_key);
            if (s_name[0]) snprintf(g_cfg.name, sizeof(g_cfg.name), "%s", s_name);
            config_save();
            pair_set_status(PAIR_OK, "");
            s_st = LS_OK; s_ok_t = 0;
        } else if (r == 0 && gem_validate_no_link()) {
            // con Wi-Fi pero sin Internet: no se pierde la clave, se vuelve a probar sola
            snprintf(s_err, sizeof(s_err), "%s", _("Sin conexión a Internet. Comprobaré la clave sola en cuanto haya conexión."));
            pair_set_status(PAIR_ERROR, s_err);
            s_st = LS_ERR; s_wait_net = true; s_retry_t = 0;
        } else if (r == 0) {
            snprintf(s_err, sizeof(s_err), "%s", msg);
            pair_set_status(PAIR_ERROR, msg);
            s_st = LS_ERR; s_wait_net = false;
        }
    }
    if (s_st == LS_OK) {
        s_ok_t += g_dt;
        if (s_ok_t > 1.0f && s_manual && !g_cfg.name[0]) {
            char nm[48] = "";
            s_manual = false;
            if (kb_input(_("¿Cómo quieres que te llame? (opcional)"), "", nm, sizeof(nm), false, false) && nm[0]) {
                snprintf(g_cfg.name, sizeof(g_cfg.name), "%s", nm);
                config_save();
            }
        }
        if (s_ok_t > 1.8f) scene_goto(SC_CHAT);
        return;
    }
    if (s_st == LS_CHECK) return;

    // botones inferiores
    if (tap(20, 112, 280, 38) || (g_kdown & KEY_A)) {
        char key[128] = "";
        if (kb_input(_("Escribe tu clave de API de Gemini"), "", key, sizeof(key), false, false)) {
            str_trim(key);
            if (strlen(key) >= 20) start_check(key, NULL, true);
            else { s_st = LS_ERR; snprintf(s_err, sizeof(s_err), _("La clave es demasiado corta.")); }
        }
    }
    if (tap(20, 158, 280, 38) || (g_kdown & KEY_Y)) s_help = !s_help;
    if (tap(20, 204, 136, 30) || (g_kdown & KEY_X)) {
        pair_stop(); pair_start();
        config_load();
        if (g_cfg.api_key[0]) start_check(g_cfg.api_key, g_cfg.name, true);
        else toast(_("Buscando de nuevo… (o pon apikey.txt en la SD)"));
    }
    if (tap(164, 204, 136, 30) || (g_kdown & KEY_START)) g_quit = true;
}

static void bg_glow(float w) {
    float t = g_time * 0.15f;
    draw_grad4(0, 140, w, 100, col_alpha(gemini_grad(t), 0), col_alpha(gemini_grad(t + 0.3f), 0),
               col_alpha(gemini_grad(t), 0.16f), col_alpha(gemini_grad(t + 0.3f), 0.16f));
}

void login_draw_top(void) {
    bg_glow(TOP_W);
    const char *url = pair_url();
    bool has_url = pair_running() && !strncmp(url, "http", 4);

    if (s_st == LS_OK) {
        float a = ease_out_back(clampf(s_ok_t / 0.6f, 0, 1));
        for (int i = 0; i < 10; i++) {   // destellos que explotan
            float ang = i * (float)M_PI / 5 + s_ok_t;
            float d = 30 + s_ok_t * 90;
            draw_sparkle(200 + cosf(ang) * d, 100 + sinf(ang) * d * 0.7f, 6 * (1 - clampf(s_ok_t / 1.6f, 0, 1)), ang, 1);
        }
        draw_sparkle(200, 100, 40 * a, s_ok_t * 2, 1);
        float ta = clampf(s_ok_t / 0.5f, 0, 1);
        char hi[96];
        if (g_cfg.name[0]) snprintf(hi, sizeof(hi), _("Hola, %s"), g_cfg.name); else snprintf(hi, sizeof(hi), _("Todo listo"));
        float w = text_width(hi, 1.0f);
        text_draw_grad(hi, 200 - w / 2, 156, 1.0f, g_time * 0.2f, ta);
        text_draw_center(_("Tu 3DS ya está conectada con Gemini"), 200, 192, 0.52f, col_alpha(T.text2, ta));
        return;
    }

    if (s_help) {
        text_draw(_("¿Cómo funciona?"), 20, 16, 0.75f, T.text);
        const char *h =
            _("Gemi3DS usa la API oficial de Gemini con tu propia clave gratuita de Google AI Studio.\n"
            "1. Escanea el QR con tu teléfono (misma red Wi-Fi que la 3DS).\n"
            "2. En la página, toca \"Abrir Google AI Studio\", inicia sesión con tu cuenta de Google y crea una clave.\n"
            "3. Pega la clave en la página y pulsa \"Conectar\".\n"
            "La clave se guarda en la SD y no tendrás que repetirlo.\n"
            "Alternativa: crea el archivo sd:/3ds/Gemi3DS/apikey.txt con la clave dentro y pulsa \"Reintentar\".");
        text_wrap_lines(h, 360, 0.47f, 14, 20, 46, T.text2, true);
        return;
    }

    // tarjeta QR
    float qx = 16, qy = 22, qs = 196;
    if (has_url) {
        float pulse = 0.5f + 0.5f * sinf(g_time * 2);
        draw_rrect(qx - 3, qy - 3, qs + 6, qs + 6, 16, col_alpha(gemini_grad(g_time * 0.1f), 0.35f + 0.25f * pulse));
        draw_qr(url, qx, qy, qs, 1);
    } else {
        draw_rrect(qx, qy, qs, qs, 14, T.surface);
        draw_icon(IC_WIFI, qx + qs / 2, qy + 62, 46, T.text3);
        text_draw_center(_("Sin conexión Wi-Fi"), qx + qs / 2, qy + 100, 0.5f, T.text2);
        text_wrap_lines(_("Conecta la consola en Configuración de la consola → Internet. El código aparecerá solo."),
                        qs - 24, 0.4f, 4, qx + 12, qy + 126, T.text3, true);
    }
    // texto lateral
    float tx = 228;
    text_draw(_("Vincula tu"), tx, 22, 0.72f, T.text);
    text_draw_grad(_("cuenta de Google"), tx, 44, 0.72f, g_time * 0.1f, 1);
    const char *steps[] = {_("Escanea el código con tu teléfono"), _("Inicia sesión y crea tu clave gratis"), _("Pégala en la página y listo")};
    for (int i = 0; i < 3; i++) {
        float y = 80 + i * 36;
        draw_circle(tx + 9, y + 9, 9, T.surface2);
        char n[4]; snprintf(n, sizeof(n), "%d", i + 1);
        text_draw_center(n, tx + 9, y + 2, 0.45f, T.accent);
        text_wrap_lines(steps[i], 140, 0.42f, 2, tx + 24, y, T.text2, true);
    }
    // estado
    float sy = 196;
    if (s_st == LS_CHECK) {
        draw_arc(tx + 8, sy + 8, 7, 2, g_time * 6, g_time * 6 + 4.2f, T.accent);
        text_draw(_("Comprobando la clave…"), tx + 22, sy + 1, 0.45f, T.text);
    } else if (s_st == LS_ERR && s_wait_net) {
        draw_arc(tx + 6, sy - 1, 5, 1.5f, g_time * 4, g_time * 4 + 4.2f, T.text3);
        text_wrap_lines(s_err, 154, 0.4f, 3, tx + 14, sy - 8, T.text2, true);
    } else if (s_st == LS_ERR) {
        text_wrap_lines(s_err, 168, 0.4f, 3, tx, sy - 8, T.danger, true);
    } else if (pair_phone_seen()) {
        draw_circle(tx + 6, sy + 8, 4, T.ok);
        text_draw(_("Teléfono conectado"), tx + 16, sy + 1, 0.45f, T.ok);
    } else if (has_url) {
        int dots = (int)(g_time * 2) % 4;
        char w[40]; snprintf(w, sizeof(w), _("Esperando al teléfono%.*s"), dots, "...");
        text_draw(w, tx, sy + 1, 0.45f, T.text3);
    }
    if (has_url) text_draw_clip(url, qx, 224, qs, 0.4f, T.text3);
}

void login_draw_bot(void) {
    bg_glow(BOT_W);
    if (s_st == LS_OK) {
        draw_icon(IC_CHECK, 160, 110, 50, T.ok);
        return;
    }
    draw_sparkle(34, 34, 14, g_time * 0.8f, 1);
    text_draw(_("Bienvenido a " APP_NAME), 56, 16, 0.62f, T.text);
    text_draw(_("Gemini en tu Nintendo 3DS"), 56, 38, 0.45f, T.text3);
    text_wrap_lines(_("Para empezar, vincula tu cuenta con el teléfono o escribe tu clave de API."), 280, 0.45f, 3, 20, 66, T.text2, true);
    bool busy = s_st == LS_CHECK;
    u32 fg = T.dark ? col_hex(0x1F1F1F) : 0xFFFFFFFF;
    ui_pill_button(_("Escribir la clave en la consola"), IC_KEY, 20, 112, 280, 38, fg, busy ? T.text3 : T.accent);
    ui_pill_button(s_help ? _("Ver el código QR") : _("¿Cómo funciona?"), s_help ? IC_QR : IC_INFO, 20, 158, 280, 38, T.text, T.surface2);
    ui_pill_button(_("Reintentar"), IC_REFRESH, 20, 204, 136, 30, T.text2, T.surface);
    ui_pill_button(_("Salir"), IC_CLOSE, 164, 204, 136, 30, T.text2, T.surface);
}

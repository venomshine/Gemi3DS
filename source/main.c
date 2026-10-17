// Gemi3DS - cliente de Gemini AI para Nintendo 3DS
// Punto de entrada: inicialización, bucle principal, pantalla de arranque y transiciones.
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "app.h"
#include "gfx.h"
#include "ui.h"
#include "net.h"
#include "store.h"
#include "gemini.h"
#include "media.h"
#include "scenes.h"

int g_scene = SC_SPLASH;
bool g_quit;
bool g_net_ok;

static int s_next = -1;
static float s_fade = 0;        // 0 = visible, 1 = negro
static float s_splash_t = 0;

void scene_goto(int s) { s_next = s; }

static void enter(int s) {
    if (g_scene == SC_LOGIN && s != SC_LOGIN) login_leave();
    g_scene = s;
    if (s == SC_LOGIN) login_enter();
    else if (s == SC_CHAT) chat_enter();
}

// ------------------------------------------------------------ splash
static void splash_top(void) {
    float t = s_splash_t;
    float a = ease_out_cubic(t / 0.9f);
    // halo de fondo
    for (int i = 0; i < 6; i++) {
        float r = 30 + i * 22 * a;
        draw_circle(200, 104, r, col_alpha(gemini_grad(i / 6.0f + t * 0.2f), 0.05f * a));
    }
    float sc = ease_out_back(clampf(t / 0.8f, 0, 1));
    draw_sparkle(200, 104, 46 * sc, t * 1.4f, 1);
    float ta = clampf((t - 0.5f) / 0.5f, 0, 1);
    float w = text_width(APP_NAME, 1.1f);
    text_draw_grad(APP_NAME, 200 - w / 2, 160 + (1 - ta) * 10, 1.1f, t * 0.3f, ta);
    text_draw_center(_("Gemini AI para Nintendo 3DS"), 200, 198, 0.5f, col_alpha(T.text3, ta));
}

static void splash_bot(void) {
    float t = s_splash_t;
    float ta = clampf((t - 0.7f) / 0.4f, 0, 1);
    // tres puntos animados
    for (int i = 0; i < 3; i++) {
        float ph = sinf(g_time * 6 - i * 0.7f) * 0.5f + 0.5f;
        draw_circle(148 + i * 12, 120, 3 + ph * 1.5f, col_alpha(gemini_grad(i / 3.0f), ta * (0.4f + ph * 0.6f)));
    }
    text_draw_center("v" APP_VERSION, 160, 214, 0.42f, col_alpha(T.text3, ta));
}

// ------------------------------------------------------------ main
int main(int argc, char **argv) {
    osSetSpeedupEnable(true);
    romfsInit();
    cfguInit();
    gfx_init();
    store_init();
    config_load();
    i18n_apply(g_cfg.lang);
    theme_set(g_cfg.dark);
    index_load();
    gem_api_init();
    hidSetRepeatParameters(18, 5);

    bool net_started = false;
    bool splash_done = false;

    while (aptMainLoop() && !g_quit) {
#ifdef G3_PERF
        u64 perf_t0 = svcGetSystemTick();
#endif
        input_update();
        gfx_tick();

        if (g_scene == SC_SPLASH) {
            s_splash_t += g_dt;
            if (!net_started && s_splash_t > 0.15f) {
                g_net_ok = net_init();
                net_started = true;
                if (g_cfg.api_key[0]) gem_models_fetch();
            }
            if (!splash_done && s_splash_t > 1.7f) {
                splash_done = true;
                scene_goto(g_cfg.api_key[0] ? SC_CHAT : SC_LOGIN);
            }
        } else if (s_fade < 0.5f || s_next < 0) {
            if (g_scene == SC_LOGIN) login_update();
            else if (g_scene == SC_CHAT) chat_update();
        }
        if (g_scene != SC_SPLASH) gem_models_poll();

        // transición
        if (s_next >= 0) {
            s_fade = approach(s_fade, 1.0f, 14);
            if (s_fade >= 0.99f) { enter(s_next); s_next = -1; }
        } else {
            s_fade = approach(s_fade, 0.0f, 10);
        }

        // la lógica (incluido el teclado del sistema) va fuera del frame de dibujo
        gfx_frame_begin();
        gfx_target(g_top, T.bg);
        if (g_scene == SC_SPLASH) splash_top();
        else if (g_scene == SC_LOGIN) login_draw_top();
        else chat_draw_top();
        toast_draw_top();
        if (s_fade > 0.01f) draw_rect(0, 0, TOP_W, SCR_H, col_alpha(T.bg, s_fade));

        gfx_target(g_bot, T.bg);
        if (g_scene == SC_SPLASH) splash_bot();
        else if (g_scene == SC_LOGIN) login_draw_bot();
        else chat_draw_bot();
        if (s_fade > 0.01f) draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.bg, s_fade));
#ifdef G3_PERF
        {   static u64 acc; static int nf;
            acc += svcGetSystemTick() - perf_t0;
            if (++nf == 120) { FILE *pf = fopen("sdmc:/perf.txt", "a");
                if (pf) { fprintf(pf, "%.2f ms\n", acc / 120.0 / 268111.856); fclose(pf); } acc = 0; nf = 0; } }
#endif

        gfx_frame_end();
    }

    chat_shutdown();
    login_leave();
    gem_stop_wait();
    cam_close();
    mic_stop();
    net_exit();
    gfx_exit();
    cfguExit();
    romfsExit();
    return 0;
}

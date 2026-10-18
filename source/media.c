/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - cámara, micrófono, audio e imágenes
#include "media.h"
#include "app.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

// ------------------------------------------------------------ imágenes
static u8 *downscale(const u8 *src, int w, int h, int nw, int nh) {
    u8 *d = malloc(nw * nh * 4);
    if (!d) return NULL;
    float sx = (float)w / nw, sy = (float)h / nh;
    for (int y = 0; y < nh; y++) {
        int y0 = (int)(y * sy), y1 = (int)((y + 1) * sy); if (y1 <= y0) y1 = y0 + 1; if (y1 > h) y1 = h;
        for (int x = 0; x < nw; x++) {
            int x0 = (int)(x * sx), x1 = (int)((x + 1) * sx); if (x1 <= x0) x1 = x0 + 1; if (x1 > w) x1 = w;
            u32 acc[4] = {0}, cnt = 0;
            int stepy = (y1 - y0) > 4 ? (y1 - y0) / 4 : 1, stepx = (x1 - x0) > 4 ? (x1 - x0) / 4 : 1;
            for (int yy = y0; yy < y1; yy += stepy)
                for (int xx = x0; xx < x1; xx += stepx) {
                    const u8 *p = src + (yy * w + xx) * 4;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2]; acc[3] += p[3]; cnt++;
                }
            u8 *o = d + (y * nw + x) * 4;
            for (int k = 0; k < 4; k++) o[k] = acc[k] / cnt;
        }
    }
    return d;
}

bool img_load_tex(const char *path, Texture *t, int maxw, int maxh) {
    size_t sz;
    u8 *f = file_read(path, &sz);
    if (!f) return false;
    int w, h, ch;
    u8 *px = stbi_load_from_memory(f, (int)sz, &w, &h, &ch, 4);
    free(f);
    if (!px) return false;
    float s = fminf((float)maxw / w, (float)maxh / h);
    if (s > 1) s = 1;
    int nw = (int)(w * s), nh = (int)(h * s);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    bool ok;
    if (nw != w || nh != h) {
        u8 *d = downscale(px, w, h, nw, nh);
        stbi_image_free(px);
        if (!d) return false;
        ok = tex_from_rgba(t, d, nw, nh);
        free(d);
    } else {
        ok = tex_from_rgba(t, px, w, h);
        stbi_image_free(px);
    }
    return ok;
}

bool img_info(const char *path, int *w, int *h) {
    size_t sz;
    u8 *f = file_read(path, &sz);
    if (!f) return false;
    int ch;
    bool ok = stbi_info_from_memory(f, (int)sz, w, h, &ch);
    free(f);
    return ok;
}

// ------------------------------------------------------------ cámara
#define CAM_W 400
#define CAM_H 240
#define CAM_BYTES (CAM_W * CAM_H * 2)
static bool s_cam;
static bool s_front;
static u16 *s_cambuf[2], *s_lastframe;
static int s_camcur;                  // búfer que está recibiendo el fotograma actual
static Handle s_camev, s_camerr;
static u32 s_camsz;
static bool s_have_frame;
static u64 s_cam_last;                // último fotograma recibido (ms)

static void cam_receive(void) {
    if (s_camev) { svcCloseHandle(s_camev); s_camev = 0; }
    CAMU_SetReceiving(&s_camev, s_cambuf[s_camcur], PORT_CAM1, CAM_BYTES, (s16)s_camsz);
}

// rearma la captura: si la app no recoge un fotograma a tiempo, CAMU desborda el búfer
// y deja de capturar (la vista previa se quedaba congelada)
static void cam_restart(void) {
    CAMU_StopCapture(PORT_CAM1);
    CAMU_ClearBuffer(PORT_CAM1);
    if (s_camerr) svcClearEvent(s_camerr);
    CAMU_StartCapture(PORT_CAM1);
    cam_receive();
    s_cam_last = osGetTime();
}

bool cam_open(bool front) {
    if (s_cam) cam_close();
    if (R_FAILED(camInit())) return false;
    u32 sel = front ? SELECT_IN1 : SELECT_OUT1;
    s_front = front;
    CAMU_SetSize(sel, SIZE_CTR_TOP_LCD, CONTEXT_A);
    CAMU_SetOutputFormat(sel, OUTPUT_RGB_565, CONTEXT_A);
    CAMU_SetFrameRate(sel, FRAME_RATE_30);
    CAMU_SetNoiseFilter(sel, true);
    CAMU_SetAutoExposure(sel, true);
    CAMU_SetAutoWhiteBalance(sel, true);
    CAMU_SetTrimming(PORT_CAM1, false);
    CAMU_GetMaxBytes(&s_camsz, CAM_W, CAM_H);
    CAMU_SetTransferBytes(PORT_CAM1, s_camsz, CAM_W, CAM_H);
    CAMU_Activate(sel);
    for (int i = 0; i < 2; i++) if (!s_cambuf[i]) s_cambuf[i] = malloc(CAM_BYTES);
    if (!s_lastframe) s_lastframe = malloc(CAM_BYTES);
    if (!s_cambuf[0] || !s_cambuf[1] || !s_lastframe) { CAMU_Activate(SELECT_NONE); camExit(); return false; }
    if (!s_camerr) CAMU_GetBufferErrorInterruptEvent(&s_camerr, PORT_CAM1);
    s_camcur = 0;
    s_cam = true;
    s_have_frame = false;
    cam_restart();
    return true;
}

void cam_close(void) {
    if (!s_cam) return;
    CAMU_StopCapture(PORT_CAM1);
    if (s_camev) { svcCloseHandle(s_camev); s_camev = 0; }
    if (s_camerr) { svcCloseHandle(s_camerr); s_camerr = 0; }
    CAMU_Activate(SELECT_NONE);
    camExit();
    s_cam = false;
}

bool cam_is_open(void) { return s_cam; }
bool cam_front(void) { return s_front; }

bool cam_update(Texture *t) {
    if (!s_cam) return false;
    // desbordamiento o sin fotogramas desde hace rato: se reinicia la captura
    if ((s_camerr && svcWaitSynchronization(s_camerr, 0) == 0) || !s_camev ||
        osGetTime() - s_cam_last > (s_have_frame ? 600u : 2000u)) {
        cam_restart();
        return false;
    }
    if (svcWaitSynchronization(s_camev, 0) != 0) return false;
    // el siguiente fotograma va al otro búfer, así la recepción no espera a la copia
    u16 *done = s_cambuf[s_camcur];
    s_camcur ^= 1;
    cam_receive();
    s_cam_last = osGetTime();
    if (s_front) {
        // espejo horizontal para la cámara interior
        for (int y = 0; y < CAM_H; y++) {
            u16 *r = done + y * CAM_W, *o = s_lastframe + y * CAM_W;
            for (int x = 0; x < CAM_W; x++) o[x] = r[CAM_W - 1 - x];
        }
    } else memcpy(s_lastframe, done, CAM_BYTES);
    s_have_frame = true;
    tex_from_rgb565(t, s_lastframe, CAM_W, CAM_H);
    return true;
}

typedef struct { u8 *d; size_t len, cap; } MemOut;
static void mem_write(void *ctx, void *data, int size) {
    MemOut *m = ctx;
    if (m->len + size > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 65536;
        while (nc < m->len + size) nc *= 2;
        m->d = realloc(m->d, nc); m->cap = nc;
    }
    memcpy(m->d + m->len, data, size);
    m->len += size;
}

bool cam_snap(char *path_out, int n) {
    if (!s_have_frame) return false;
    u8 *rgb = malloc(CAM_W * CAM_H * 3);
    if (!rgb) return false;
    for (int i = 0; i < CAM_W * CAM_H; i++) {
        u16 p = s_lastframe[i];
        u8 r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        rgb[i * 3 + 0] = (r << 3) | (r >> 2);
        rgb[i * 3 + 1] = (g << 2) | (g >> 4);
        rgb[i * 3 + 2] = (b << 3) | (b >> 2);
    }
    MemOut mo = {0};
    int ok = stbi_write_jpg_to_func(mem_write, &mo, CAM_W, CAM_H, 3, rgb, 90);
    free(rgb);
    if (!ok || !mo.d) { free(mo.d); return false; }
    char id[24]; make_id(id, sizeof(id));
    snprintf(path_out, n, MEDIA_DIR "/foto_%s.jpg", id);
    bool w = file_write(path_out, mo.d, mo.len);
    free(mo.d);
    return w;
}

// ------------------------------------------------------------ micrófono
#define MIC_BUFSZ 0x30000
#define MIC_RATE 16360
#define MIC_MAX_SEC 30
static u8 *s_micbuf;
static bool s_mic;
static u32 s_mic_datasz, s_mic_pos;
static s16 *s_rec; static size_t s_rec_n, s_rec_cap;
static float s_level;

bool mic_start(void) {
    if (s_mic) return true;
    if (!s_micbuf) s_micbuf = memalign(0x1000, MIC_BUFSZ);
    if (!s_micbuf) return false;
    if (R_FAILED(micInit(s_micbuf, MIC_BUFSZ))) return false;
    s_mic_datasz = micGetSampleDataSize();
    if (R_FAILED(MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED, MICU_SAMPLE_RATE_16360, 0, s_mic_datasz, true))) {
        micExit(); return false;
    }
    MICU_SetAllowShellClosed(false);
    s_mic_pos = 0;
    s_rec_n = 0;
    s_level = 0;
    s_mic = true;
    return true;
}

void mic_update(void) {
    if (!s_mic) return;
    u32 cur = micGetLastSampleOffset();
    if (s_mic_datasz == 0) return;
    if (cur >= s_mic_datasz) cur = 0;          // el final del búfer equivale al inicio
    cur &= ~1u;
    if (cur == s_mic_pos) return;
    float peak = 0;
    for (int guard = 0; guard < 2 && s_mic_pos != cur; guard++) {
        u32 end = cur > s_mic_pos ? cur : s_mic_datasz;
        u32 nbytes = end - s_mic_pos;
        size_t ns = nbytes / 2;
        if (s_rec_n + ns > s_rec_cap) {
            s_rec_cap = (s_rec_n + ns) * 2 + 16384;
            s_rec = realloc(s_rec, s_rec_cap * 2);
        }
        s16 *src = (s16 *)(s_micbuf + s_mic_pos);
        for (size_t i = 0; i < ns; i++) {
            s_rec[s_rec_n + i] = src[i];
            float a = fabsf(src[i] / 32768.0f);
            if (a > peak) peak = a;
        }
        s_rec_n += ns;
        s_mic_pos = end >= s_mic_datasz ? 0 : end;
    }
    s_level = s_level * 0.6f + peak * 0.4f;
    if (s_rec_n > (size_t)MIC_RATE * MIC_MAX_SEC) mic_stop();
}

void mic_stop(void) {
    if (!s_mic) return;
    MICU_StopSampling();
    micExit();
    s_mic = false;
}

bool mic_active(void) { return s_mic; }
float mic_level(void) { return s_level; }
float mic_seconds(void) { return (float)s_rec_n / MIC_RATE; }

bool mic_save_wav(char *path_out, int n) {
    if (s_rec_n < MIC_RATE / 2) return false;   // menos de medio segundo
    u32 data = s_rec_n * 2;
    u8 *buf = malloc(44 + data);
    if (!buf) return false;
    memcpy(buf, "RIFF", 4);
    *(u32 *)(buf + 4) = 36 + data;
    memcpy(buf + 8, "WAVEfmt ", 8);
    *(u32 *)(buf + 16) = 16;
    *(u16 *)(buf + 20) = 1;
    *(u16 *)(buf + 22) = 1;
    *(u32 *)(buf + 24) = MIC_RATE;
    *(u32 *)(buf + 28) = MIC_RATE * 2;
    *(u16 *)(buf + 32) = 2;
    *(u16 *)(buf + 34) = 16;
    memcpy(buf + 36, "data", 4);
    *(u32 *)(buf + 40) = data;
    memcpy(buf + 44, s_rec, data);
    char id[24]; make_id(id, sizeof(id));
    snprintf(path_out, n, MEDIA_DIR "/voz_%s.wav", id);
    bool ok = file_write(path_out, buf, 44 + data);
    free(buf);
    return ok;
}


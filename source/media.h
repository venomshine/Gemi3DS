/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - cámara, micrófono, audio e imágenes
#pragma once
#include "gfx.h"

// imágenes (PNG/JPG desde archivo) -> textura reducida
bool img_load_tex(const char *path, Texture *t, int maxw, int maxh);
bool img_info(const char *path, int *w, int *h);

// cámara
bool cam_open(bool front);
void cam_close(void);
bool cam_is_open(void);
bool cam_update(Texture *t);                 // true si llegó un frame nuevo
bool cam_snap(char *path_out, int n);        // guarda JPG del último frame
bool cam_front(void);

// micrófono
bool  mic_start(void);
void  mic_update(void);
void  mic_stop(void);
bool  mic_active(void);
float mic_level(void);
float mic_seconds(void);
bool  mic_save_wav(char *path_out, int n);


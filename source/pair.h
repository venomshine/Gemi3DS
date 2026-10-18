/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - vinculación desde el teléfono y mensajes compartidos (servidor HTTP local + QR)
#pragma once
#include <3ds.h>
#include <stdbool.h>

enum { PAIR_WAITING, PAIR_CHECKING, PAIR_OK, PAIR_ERROR };

bool pair_start(void);
void pair_stop(void);
bool pair_running(void);
void pair_poll(void);                       // atender peticiones (no bloqueante)
const char *pair_url(void);                 // http://ip:puerto/
bool pair_take(char *key, int kn, char *name, int nn);   // true si llegó una clave nueva
void pair_set_status(int state, const char *msg);
bool pair_phone_seen(void);                 // el teléfono abrió la página
// sirve el texto en http://ip:puerto/m/ID para abrirlo en el teléfono (false si no hay Wi-Fi)
bool pair_share(const char *text, char *url, int n);

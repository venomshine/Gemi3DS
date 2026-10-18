/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once
#include <3ds.h>
#include <stddef.h>
#include <stdbool.h>

char *b64_encode(const u8 *data, size_t len, size_t *out_len);
u8   *b64_decode(const char *s, size_t len, size_t *out_len);
u8   *file_read(const char *path, size_t *size);          // añade NUL al final
bool  file_write(const char *path, const void *data, size_t size);
void  mkdirs(const char *path);
bool  file_exists(const char *path);
void  make_id(char *out, int n);
void  fmt_when(u64 ms, char *out, int n);                 // "Hoy 14:05", "12 oct"
void  str_trim(char *s);
void  url_decode(char *s);

/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - maquetador de Markdown ligero para la pantalla de 3DS
#pragma once
#include <3ds.h>
#include <stdbool.h>

enum { MS_BOLD = 1, MS_ITALIC = 2, MS_CODE = 4, MS_LINK = 8 };
enum { LK_TEXT, LK_CODE, LK_QUOTE, LK_HR, LK_HEAD, LK_TABLE };

typedef struct { int off, len; float x, w; u8 flags; } MdSeg;
typedef struct {
    float y, h, scale, indent;
    int seg0, nseg;
    u8 kind, code_first, code_last;
    int marker_off, marker_len;   // viñeta / número (en arena), -1 si no hay
    float marker_x;
} MdLine;

typedef struct {
    char *arena; int alen, acap;
    MdSeg *segs; int nseg, cseg;
    MdLine *lines; int nline, cline;
    float height, width, maxw;
} MdLayout;

void md_layout(MdLayout *L, const char *text, float width, float scale, bool markdown);
void md_draw(const MdLayout *L, float x, float y, float clip_top, float clip_bot, u32 col, float alpha);
void md_free(MdLayout *L);
// convierte markdown a texto plano (para títulos, TTS, QR)
void md_strip(const char *in, char *out, int outsz);

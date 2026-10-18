/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Gemi3DS - uso de los modelos (contado en la consola: Google no informa del cupo restante)
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <time.h>

#define USAGE_RECENT 32

typedef struct {
    char id[80];
    int  day;                          // día del cupo (AAAAMMDD, hora del Pacífico)
    int  req, ok, limited;             // peticiones hoy, correctas y rechazadas por cupo (429)
    int  tok_in, tok_out;              // tokens hoy (entrada y salida, razonamiento incluido)
    int  limit_day, limit_min;         // límites que Google indicó en un 429 (0 = desconocido)
    int  last_code, last_ms;           // último código HTTP y tiempo hasta el primer byte
    u64  last_time;                    // osGetTime() de la última petición
    u64  recent[USAGE_RECENT];         // marcas de tiempo para contar peticiones por minuto
} UsageModel;

void usage_load(void);
void usage_set_utc(time_t utc);        // hora de los servidores (cabecera Date) para el día del cupo
void usage_record(const char *model, long http_code, int tok_in, int tok_out, int first_ms, const char *resp);
int  usage_count(void);
UsageModel *usage_get(int i);          // ordenados por peticiones de hoy
UsageModel *usage_find(const char *model);
int  usage_rpm(const UsageModel *u);   // peticiones en los últimos 60 s
void usage_set_limit(const char *model, int per_day);   // límite diario indicado a mano (0 = desconocido)
float usage_left(const UsageModel *u); // fracción del límite diario que queda (0-1), o -1 si no se conoce
void usage_totals(int *req, int *tokens);
bool usage_reset_local(int *hh, int *mm);   // hora local de la consola en que se reinicia el cupo

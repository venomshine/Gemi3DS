/*
 * Gemi3DS - cliente de Gemini AI para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once
#include "app.h"

void store_init(void);
void config_load(void);
void config_save(void);
void index_load(void);
void index_save(void);
void index_touch(Chat *c);              // actualiza/inserta meta del chat
void index_remove(const char *id);
void index_sort(void);
bool chat_save(Chat *c);
Chat *chat_load(const char *id);
void chat_delete_file(const char *id);
void history_clear_all(void);

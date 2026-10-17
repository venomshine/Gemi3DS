// Gemi3DS - cliente de la API de Gemini (generativelanguage.googleapis.com)
#pragma once
#include "app.h"

void  gem_api_init(void);
const char *gem_current_model(void);     // modelo que se usará en el próximo mensaje
const char *gem_mode_label(int mode);
bool  gem_mode_auto(void);               // Rápido/Razonamiento: elige y cambia de modelo solo

// modelos
void  gem_models_fetch(void);
bool  gem_models_busy(void);
void  gem_models_poll(void);

// validación de clave (vinculación)
void  gem_validate_key(const char *key);
int   gem_validate_poll(char *msg, int n);    // -1 pendiente, 0 falló, 1 ok

// chat
bool  gem_send(Chat *c, Msg *target);
void  gem_stop(void);
void  gem_stop_wait(void);
bool  gem_streaming(void);
Msg  *gem_target(void);
const char *gem_status(void);             // texto de estado ("Probando con…") o ""
int   gem_poll(void);                     // 1 si terminó en este frame
int   gem_take_memory_event(void);        // >0 recuerdos guardados, <0 olvidados (una vez)

// título automático
void  gem_title_request(Chat *c);
void  gem_title_poll(void);

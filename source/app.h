// Gemi3DS - modelo de datos compartido
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include "gfx.h"
#include "md.h"
#include "i18n.h"

#define APP_NAME    "Gemi3DS"
#define APP_VERSION "1.0.0"
#define APP_AUTHOR  "VENOMSHiNE"
#define APP_DIR     "sdmc:/3ds/Gemi3DS"
#define CHATS_DIR   APP_DIR "/chats"
#define MEDIA_DIR   APP_DIR "/media"

// ------------------------------------------------------------ configuración
enum { MODE_FAST, MODE_THINK, MODE_PRO, MODE_CUSTOM };
enum { TS_NORMAL, TS_LARGE, TS_SMALL };

typedef struct {
    char api_key[128];
    char name[48];
    int  mode;
    char custom_model[80];
    bool dark;
    int  text_size;
    bool show_thoughts;
    char saved_info[3000];     // "Información guardada": un recuerdo por línea
    char api_base[160];        // (avanzado) URL base de la API; vacío = Google
    int  lang;                 // 0 sistema, 1 español, 2 inglés, 3 japonés
} Config;

extern Config g_cfg;

// ------------------------------------------------------------ modelos
typedef struct { char id[80]; char name[64]; char desc[160]; } ModelInfo;
typedef struct {
    char flash[80], pro[80], lite[80];
    ModelInfo list[48]; int n;
    bool loaded;
} Models;
extern Models g_models;


// ------------------------------------------------------------ chats
enum { ROLE_USER, ROLE_MODEL };
enum { MST_DONE, MST_STREAM, MST_ERROR, MST_STOPPED };
enum { ATT_NONE, ATT_IMAGE, ATT_AUDIO };

typedef struct {
    int role;
    char *text;    int tlen, tcap;
    char *thought; int thlen, thcap;
    char *sig;                        // thoughtSignature
    int  att;                         // adjunto
    char att_path[160];               // jpg/png/wav en la SD
    char att_mime[32];
    char model[80];
    int state;
    volatile int ver;
    u64 time;
    // --- UI (no se guarda)
    MdLayout lay; int lay_ver; float lay_w, lay_scale;
    MdLayout tlay; int tlay_ver;
    bool show_thought;
    float appear;
    float y, h;
    Texture thumb; bool thumb_tried;
} Msg;

typedef struct {
    char id[24];
    char title[100];
    bool pinned, temporary, titled;
    u64 updated;
    Msg **m; int n, cap;
} Chat;

typedef struct { char id[24]; char title[100]; bool pinned; u64 updated; } ChatMeta;

extern Chat *g_chat;
extern LightLock g_chat_lock;
extern ChatMeta g_index[300];
extern int g_nindex;

Msg  *msg_new(int role);
void  msg_free(Msg *m);
void  msg_append(Msg *m, const char *s, int n);       // con bloqueo externo
void  msg_append_thought(Msg *m, const char *s, int n);
void  msg_set_text(Msg *m, const char *s);
Chat *chat_new(void);
void  chat_free(Chat *c);
void  chat_push(Chat *c, Msg *m);
void  chat_truncate(Chat *c, int n);                  // deja n mensajes

// ------------------------------------------------------------ información guardada (memoria)
int  mem_count(void);
bool mem_get(int i, char *out, int n);
void mem_add(const char *s);
void mem_set(int i, const char *s);
void mem_delete(int i);
int  mem_forget(const char *needle);     // borra los recuerdos que contengan el texto
void mem_clear(void);

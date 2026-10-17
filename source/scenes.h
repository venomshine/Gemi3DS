#pragma once
#include <stdbool.h>

enum { SC_SPLASH, SC_LOGIN, SC_CHAT };
extern int g_scene;
extern bool g_quit;
extern bool g_net_ok;
void scene_goto(int s);

void login_enter(void);
void login_leave(void);
void login_update(void);
void login_draw_top(void);
void login_draw_bot(void);

void chat_enter(void);
void chat_update(void);
void chat_draw_top(void);
void chat_draw_bot(void);
void chat_shutdown(void);

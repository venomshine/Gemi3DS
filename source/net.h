// Gemi3DS - red: hilo de trabajo con libcurl + mbedTLS
#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum { REQ_IDLE, REQ_QUEUED, REQ_RUNNING, REQ_DONE, REQ_FAILED } ReqState;

typedef struct NetReq NetReq;
typedef void (*net_data_cb)(NetReq *r, const char *data, size_t len);

struct NetReq {
    char url[600];
    char *body;              // si no es NULL -> POST (lo libera el dueño)
    char api_key[128];
    net_data_cb on_data;     // si se define, se llama con cada bloque (streaming)
    void *user;
    // resultado
    volatile ReqState state;
    volatile bool cancel;
    long http_code;
    char *resp; size_t resp_len, resp_cap;   // cuerpo (si no hay on_data, o si http != 200)
    char err[256];
    int curl_code;
    // tiempos
    int  first_byte_timeout_ms;   // 0 = sin límite; si se supera sin recibir nada, se aborta
    volatile bool fb_timed_out;
    u64  t_start, t_first;        // ms (osGetTime)
};

bool net_init(void);
void net_exit(void);
bool net_submit(NetReq *r);      // encola (r debe vivir hasta DONE/FAILED)
void net_req_reset(NetReq *r);   // libera resp y deja en IDLE
bool net_busy(const NetReq *r);
u32  net_local_ip(void);
const char *net_strerror(NetReq *r);

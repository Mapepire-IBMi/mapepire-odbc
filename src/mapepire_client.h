/*
 * Mapepire protocol client: one WebSocket, JSON requests correlated by id.
 *
 * Thread-safe: any number of threads may have requests in flight on one
 * client at the same time.  A reader thread routes each reply to the thread
 * waiting for its id; replies to fire-and-forget requests (e.g. sqlclose)
 * are dropped.  See mapepire_client.c.
 */
#ifndef MAPEPIRE_CLIENT_H
#define MAPEPIRE_CLIENT_H

#include "mapepire_json.h"
#include "mapepire_ws.h"

typedef enum {
    MAPEPIRE_ERR_NONE = 0,
    MAPEPIRE_ERR_SERVER,      /* server replied success:false */
    MAPEPIRE_ERR_CONNECT,     /* could not open the WebSocket */
    MAPEPIRE_ERR_AUTH,        /* credentials rejected */
    MAPEPIRE_ERR_TLS,
    MAPEPIRE_ERR_COMM,        /* link failure after connecting */
    MAPEPIRE_ERR_TIMEOUT,
    MAPEPIRE_ERR_PROTOCOL,    /* unparseable reply */
    MAPEPIRE_ERR_MEMORY
} mapepire_err_kind;

typedef struct mapepire_error {
    mapepire_err_kind kind;
    char        sqlstate[6];  /* "" when the server gave none */
    long        native;       /* sql_rc */
    char        msg[1024];
} mapepire_error;

typedef struct mapepire_client mapepire_client;

typedef struct mapepire_connect_opts {
    mapepire_ws_params ws;
    const char  *props;        /* JTOpen properties "k=v;k=v" */
    const char  *technique;    /* "tcp" / "cli" / NULL */
    const char  *application;
} mapepire_connect_opts;

int  mapepire_client_open(mapepire_client **_out, const mapepire_connect_opts *_o, mapepire_error *_e);
void mapepire_client_close(mapepire_client *_c);   /* sends exit (best effort) */

/*
 * Send req (ownership taken; "id" is assigned and copied into id_out when
 * non-NULL, capacity 32) and wait for the matching response.  Returns the
 * response (caller frees) or NULL with e filled.  A response with
 * success:false is converted into an MAPEPIRE_ERR_SERVER error and freed.
 */
mpjson *mapepire_client_call(mapepire_client *_c, mpjson *_req, char *_id_out, mapepire_error *_e);

/* As mapepire_client_call, waiting at most timeout_ms (MAPEPIRE_CALL_DEFAULT_TIMEOUT = the
 * connection's SOCKETTIMEOUT).  A timed-out request leaves the connection usable;
 * its late reply is dropped. */
#define MAPEPIRE_CALL_DEFAULT_TIMEOUT (-2L)
mpjson *mapepire_client_call_timeout(mapepire_client *_c, mpjson *_req, char *_id_out, long _timeout_ms, mapepire_error *_e);

/* Send without waiting (the reply is discarded later). */
void mapepire_client_post(mapepire_client *_c, mpjson *_req);

/* Re-run "connect" with new properties on the existing WebSocket. */
int  mapepire_client_reconnect(mapepire_client *_c, const char *_props, mapepire_error *_e);

/* Fetch a blob_url path over HTTP. */
int  mapepire_client_http_get(mapepire_client *_c, const char *_path, mapepire_buf *_out, mapepire_error *_e);

int         mapepire_client_is_dead(mapepire_client *_c);
const char *mapepire_client_job(mapepire_client *_c);
const char *mapepire_client_version(mapepire_client *_c);

/* Build a request object of the given type (id is assigned on send). */
mpjson *mapepire_req(const char *_type);

void mapepire_error_set(mapepire_error *_e, mapepire_err_kind _k, const char *_state, const char *_fmt, ...);

#endif

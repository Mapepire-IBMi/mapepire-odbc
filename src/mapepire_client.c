/*
 * Mapepire protocol client.
 *
 * A Mapepire server processes requests concurrently and may answer them in any
 * order, so each connection runs a reader thread (the counterpart of the
 * EventEmitter in the JavaScript client):
 *
 *   caller thread                       reader thread
 *   -------------                       -------------
 *   register waiter for id N            loop: receive a message
 *   send request N   (send lock)              parse, look up waiter by id
 *   wait on cond until reply N  <------       hand the reply over, broadcast
 *
 * Any number of threads may have requests in flight on one connection.
 * Replies nobody is waiting for (fire-and-forget posts, requests that timed
 * out) are dropped.  If the transport fails, every waiter is woken with a
 * communication-link error and the connection is marked dead.
 */
#include "mapepire_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#endif

/* ---- optional protocol trace (MAPEPIRE_ODBC_TRACE=<file>) ---------------------- */

static void trace(const char *_what, const char *_data, size_t _len)
{
    static int checked = 0;
    static char path[1024];
    if (!checked) {
        const char *p = getenv("MAPEPIRE_ODBC_TRACE");
        mapepire_snprintf(path, sizeof path, "%s", p ? p : "");
        checked = 1;
    }
    if (!path[0]) return;
    static mapepire_mutex lock;
    static volatile int lock_init = 0;
    if (!lock_init) { mapepire_mutex_init(&lock); lock_init = 1; }
    mapepire_mutex_lock(&lock);
    FILE *f = fopen(path, "a");
    if (f) {
#ifdef _WIN32
        unsigned long tid = (unsigned long)GetCurrentThreadId();
#else
        unsigned long tid = (unsigned long)pthread_self();
#endif
        size_t n = _len > 4000 ? 4000 : _len;
        fprintf(f, "%lld [%lu] %-7s %.*s%s\n", mapepire_now_ms(), tid, _what, (int)n, _data ? _data : "", n < _len ? "...(truncated)" : "");
        fclose(f);
    }
    mapepire_mutex_unlock(&lock);
}

typedef struct waiter {
    char           id[32];
    mpjson           *resp;     /* set by the reader */
    struct waiter *next;
} waiter;

struct mapepire_client {
    mapepire_ws        *ws;
    mapepire_mutex      lock;          /* protects everything below */
    mapepire_cond       cond;          /* signalled on every delivery and on death */
    waiter       *waiters;
    unsigned long next_id;
    int           dead;
    mapepire_err_kind   dead_kind;
    char          dead_msg[512];
    int           reader_started;
    int           reader_done;
    mapepire_thread     reader;
    int           io_timeout_ms; /* per-request wait, -1 = forever */
    char          job[64];
    char          version[32];
    char         *technique;
    char         *application;
};

void mapepire_error_set(mapepire_error *_e, mapepire_err_kind _k, const char *_state, const char *_fmt, ...)
{
    if (!_e) return;
    _e->kind = _k;
    mapepire_snprintf(_e->sqlstate, sizeof _e->sqlstate, "%s", _state ? _state : "");
    _e->native = 0;
    va_list ap;
    va_start(ap, _fmt);
    vsnprintf(_e->msg, sizeof _e->msg, _fmt, ap);
    va_end(ap);
    _e->msg[sizeof _e->msg - 1] = '\0';
}

static mapepire_err_kind kind_from_ws(int _rc)
{
    switch (_rc) {
    case MAPEPIRE_WS_ECONNECT: return MAPEPIRE_ERR_CONNECT;
    case MAPEPIRE_WS_EAUTH: return MAPEPIRE_ERR_AUTH;
    case MAPEPIRE_WS_ETLS: return MAPEPIRE_ERR_TLS;
    case MAPEPIRE_WS_ETIMEOUT: return MAPEPIRE_ERR_TIMEOUT;
    case MAPEPIRE_WS_EUNSUPPORTED: return MAPEPIRE_ERR_CONNECT;
    default: return MAPEPIRE_ERR_COMM;
    }
}

mpjson *mapepire_req(const char *_type)
{
    mpjson *o = mpjson_new(MPJSON_OBJECT);
    if (o) {
        mpjson_set(o, "id", mpjson_new_string(""));
        mpjson_set(o, "type", mpjson_new_string(_type));
    }
    return o;
}

/* Must hold the lock. */
static void mark_dead_locked(mapepire_client *_c, mapepire_err_kind _kind, const char *_msg)
{
    if (_c->dead) return;
    _c->dead = 1;
    _c->dead_kind = _kind;
    mapepire_snprintf(_c->dead_msg, sizeof _c->dead_msg, "%s", _msg && *_msg ? _msg : "The connection to the server was lost");
    mapepire_cond_broadcast(&_c->cond);
}

static void dead_error(mapepire_client *_c, mapepire_error *_e)
{
    mapepire_error_set(_e, MAPEPIRE_ERR_COMM, "08S01", "Communication link failure: %s", _c->dead_msg);
}

/* ---- reader thread ------------------------------------------------------------- */

static void reader_main(void *_arg)
{
    mapepire_client *c = (mapepire_client *)_arg;
    mapepire_buf in;
    mapepire_buf_init(&in);
    for (;;) {
        char err[512] = "";
        int rc = mapepire_ws_recv(c->ws, &in, err, sizeof err);
        if (rc == MAPEPIRE_WS_ETIMEOUT) continue; /* an idle connection is not an error */
        if (rc) {
            trace("DEAD", err, strlen(err));
            mapepire_mutex_lock(&c->lock);
            mark_dead_locked(c, kind_from_ws(rc), err);
            mapepire_mutex_unlock(&c->lock);
            break;
        }
        char perr[128];
        mpjson *msg = mpjson_parse(in.data ? in.data : "", in.len, perr, sizeof perr);
        const char *id = msg && msg->type == MPJSON_OBJECT ? mpjson_get_str(msg, "id") : NULL;
        if (!id) {
            mpjson_free(msg); /* nothing we could route */
            continue;
        }
        mapepire_mutex_lock(&c->lock);
        waiter *w = c->waiters;
        while (w && (w->resp || strcmp(w->id, id))) w = w->next;
        if (w) {
            w->resp = msg;
            msg = NULL;
            mapepire_cond_broadcast(&c->cond);
        }
        mapepire_mutex_unlock(&c->lock);
        trace(msg ? "DROP" : "RECV", in.data, in.len);
        mpjson_free(msg); /* reply to a post or to a request that gave up waiting */
    }
    mapepire_buf_free(&in);
    mapepire_mutex_lock(&c->lock);
    c->reader_done = 1;
    mapepire_cond_broadcast(&c->cond);
    mapepire_mutex_unlock(&c->lock);
}

/* ---- requests ---------------------------------------------------------------- */

static void unlink_waiter_locked(mapepire_client *_c, waiter *_w)
{
    for (waiter **pp = &_c->waiters; *pp; pp = &(*pp)->next) {
        if (*pp == _w) {
            *pp = _w->next;
            return;
        }
    }
}

/* Assign an id, optionally register a waiter, and send.  Returns 0 or -1. */
static int send_request(mapepire_client *_c, mpjson *_req, waiter *_w, char *_id, mapepire_error *_e)
{
    mapepire_mutex_lock(&_c->lock);
    if (_c->dead) {
        dead_error(_c, _e);
        mapepire_mutex_unlock(&_c->lock);
        return -1;
    }
    snprintf(_id, 32, "odbc%lu", ++_c->next_id);
    if (_w) {
        memcpy(_w->id, _id, strlen(_id) + 1);
        _w->resp = NULL;
        _w->next = _c->waiters;
        _c->waiters = _w; /* registered before sending: the reply may come back at once */
    }
    mapepire_mutex_unlock(&_c->lock);

    mpjson_set(_req, "id", mpjson_new_string(_id));
    mapepire_buf b;
    mapepire_buf_init(&b);
    if (mpjson_write(_req, &b)) {
        mapepire_buf_free(&b);
        mapepire_error_set(_e, MAPEPIRE_ERR_MEMORY, "HY001", "Out of memory");
        goto fail;
    }
    char err[512] = "";
    trace(_w ? "SEND" : "POST", b.data, b.len);
    int rc = mapepire_ws_send(_c->ws, b.data, b.len, err, sizeof err);
    mapepire_buf_free(&b);
    if (rc) {
        mapepire_mutex_lock(&_c->lock);
        if (rc != MAPEPIRE_WS_ETIMEOUT) mark_dead_locked(_c, kind_from_ws(rc), err);
        mapepire_mutex_unlock(&_c->lock);
        mapepire_error_set(_e, kind_from_ws(rc), rc == MAPEPIRE_WS_ETIMEOUT ? "HYT00" : "08S01", "%s", err);
        goto fail;
    }
    return 0;
fail:
    if (_w) {
        mapepire_mutex_lock(&_c->lock);
        unlink_waiter_locked(_c, _w);
        mapepire_mutex_unlock(&_c->lock);
    }
    return -1;
}

/* Wait for w's reply. Returns the response (owned by the caller) or NULL. */
static mpjson *wait_reply(mapepire_client *_c, waiter *_w, long _timeout_ms, mapepire_error *_e)
{
    long long deadline = _timeout_ms >= 0 ? mapepire_now_ms() + _timeout_ms : 0;
    mapepire_mutex_lock(&_c->lock);
    while (!_w->resp && !_c->dead) {
        long left = -1;
        if (_timeout_ms >= 0) {
            long long now = mapepire_now_ms();
            if (now >= deadline) break;
            left = (long)(deadline - now);
        }
        mapepire_cond_wait(&_c->cond, &_c->lock, left);
    }
    mpjson *resp = _w->resp;
    unlink_waiter_locked(_c, _w);
    if (!resp) {
        trace(_c->dead ? "LOST" : "TIMEOUT", _w->id, strlen(_w->id));
        if (_c->dead) dead_error(_c, _e);
        else mapepire_error_set(_e, MAPEPIRE_ERR_TIMEOUT, "HYT00", "Timed out after %ld ms waiting for the server", _timeout_ms);
    }
    mapepire_mutex_unlock(&_c->lock);
    return resp;
}

static mpjson *check_success(mpjson *_resp, mapepire_error *_e)
{
    if (mpjson_get_bool(_resp, "success", 0)) return _resp;
    const char *msg = mpjson_get_str(_resp, "error");
    const char *st = mpjson_get_str(_resp, "sql_state");
    mapepire_error_set(_e, MAPEPIRE_ERR_SERVER, st && strlen(st) == 5 ? st : "", "%s",
                 msg ? msg : "The server reported an error without a message");
    _e->native = (long)mpjson_get_int(_resp, "sql_rc", 0);
    mpjson_free(_resp);
    return NULL;
}

static mpjson *call_timeout(mapepire_client *_c, mpjson *_req, char *_id_out, long _timeout_ms, mapepire_error *_e)
{
    if (!_req) {
        mapepire_error_set(_e, MAPEPIRE_ERR_MEMORY, "HY001", "Out of memory");
        return NULL;
    }
    waiter w;
    memset(&w, 0, sizeof w);
    char id[32] = "";
    int rc = send_request(_c, _req, &w, id, _e);
    mpjson_free(_req);
    if (_id_out) memcpy(_id_out, id, strlen(id) + 1);
    if (rc) return NULL;
    mpjson *resp = wait_reply(_c, &w, _timeout_ms, _e);
    return resp ? check_success(resp, _e) : NULL;
}

mpjson *mapepire_client_call(mapepire_client *_c, mpjson *_req, char *_id_out, mapepire_error *_e)
{
    return call_timeout(_c, _req, _id_out, _c->io_timeout_ms, _e);
}

mpjson *mapepire_client_call_timeout(mapepire_client *_c, mpjson *_req, char *_id_out, long _timeout_ms, mapepire_error *_e)
{
    return call_timeout(_c, _req, _id_out, _timeout_ms == MAPEPIRE_CALL_DEFAULT_TIMEOUT ? _c->io_timeout_ms : _timeout_ms, _e);
}

void mapepire_client_post(mapepire_client *_c, mpjson *_req)
{
    if (!_req) return;
    char id[32] = "";
    mapepire_error e;
    send_request(_c, _req, NULL, id, &e);
    mpjson_free(_req);
}

/* ---- lifecycle ---------------------------------------------------------------- */

static mpjson *connect_req(mapepire_client *_c, const char *_props)
{
    mpjson *r = mapepire_req("connect");
    if (_props && *_props) mpjson_set(r, "props", mpjson_new_string(_props));
    if (_c->technique && *_c->technique) mpjson_set(r, "technique", mpjson_new_string(_c->technique));
    if (_c->application && *_c->application) mpjson_set(r, "application", mpjson_new_string(_c->application));
    return r;
}

int mapepire_client_open(mapepire_client **_out, const mapepire_connect_opts *_o, mapepire_error *_e)
{
    *_out = NULL;
    mapepire_client *c = (mapepire_client *)calloc(1, sizeof *c);
    if (!c) {
        mapepire_error_set(_e, MAPEPIRE_ERR_MEMORY, "HY001", "Out of memory");
        return -1;
    }
    mapepire_mutex_init(&c->lock);
    mapepire_cond_init(&c->cond);
    c->technique = mapepire_strdup(_o->technique);
    c->application = mapepire_strdup(_o->application);
    c->io_timeout_ms = _o->ws.io_timeout > 0 ? _o->ws.io_timeout * 1000 : -1;

    char err[768] = "";
    int rc = mapepire_ws_open(&c->ws, &_o->ws, err, sizeof err);
    if (rc) {
        const char *state = rc == MAPEPIRE_WS_EAUTH ? "28000" : "08001";
        mapepire_error_set(_e, kind_from_ws(rc), state, "%s", err[0] ? err : "Could not connect to the Mapepire server");
        mapepire_client_close(c);
        return -1;
    }
    if (mapepire_thread_start(&c->reader, reader_main, c)) {
        mapepire_error_set(_e, MAPEPIRE_ERR_CONNECT, "08001", "Could not start the connection's reader thread");
        mapepire_client_close(c);
        return -1;
    }
    c->reader_started = 1;

    /* The handshake requests are bounded even when SOCKETTIMEOUT is unlimited. */
    long hs_timeout = _o->ws.connect_timeout > 0 ? _o->ws.connect_timeout * 1000L : 60000L;
    if (c->io_timeout_ms > hs_timeout) hs_timeout = c->io_timeout_ms;
    mpjson *resp = call_timeout(c, connect_req(c, _o->props), NULL, hs_timeout, _e);
    if (!resp) {
        /* The real server checks the password here rather than at the upgrade. */
        if (_e->kind == MAPEPIRE_ERR_SERVER && !_e->sqlstate[0]) mapepire_snprintf(_e->sqlstate, sizeof _e->sqlstate, "08001");
        mapepire_client_close(c);
        return -1;
    }
    mapepire_snprintf(c->job, sizeof c->job, "%s", mpjson_get_str(resp, "job") ? mpjson_get_str(resp, "job") : "");
    mpjson_free(resp);

    mapepire_error ve;
    resp = call_timeout(c, mapepire_req("getversion"), NULL, hs_timeout, &ve);
    if (resp) {
        const char *v = mpjson_get_str(resp, "version");
        mapepire_snprintf(c->version, sizeof c->version, "%s", v ? v : "");
        mpjson_free(resp);
    }
    *_out = c;
    return 0;
}

int mapepire_client_reconnect(mapepire_client *_c, const char *_props, mapepire_error *_e)
{
    mpjson *resp = mapepire_client_call(_c, connect_req(_c, _props), NULL, _e);
    if (!resp) return -1;
    const char *job = mpjson_get_str(resp, "job");
    if (job) {
        mapepire_mutex_lock(&_c->lock);
        mapepire_snprintf(_c->job, sizeof _c->job, "%s", job);
        mapepire_mutex_unlock(&_c->lock);
    }
    mpjson_free(resp);
    return 0;
}

void mapepire_client_close(mapepire_client *_c)
{
    if (!_c) return;
    if (_c->ws && _c->reader_started) {
        mapepire_mutex_lock(&_c->lock);
        int dead = _c->dead;
        mapepire_mutex_unlock(&_c->lock);
        if (!dead) {
            /* Ask the server to end the session; it then closes the WebSocket. */
            mapepire_error e;
            mpjson_free(call_timeout(_c, mapepire_req("exit"), NULL, 5000, &e));
        }
        /* Give the reader a moment to see the close, then force it out. */
        mapepire_mutex_lock(&_c->lock);
        long long deadline = mapepire_now_ms() + 2000;
        while (!_c->reader_done) {
            long long now = mapepire_now_ms();
            if (now >= deadline) break;
            mapepire_cond_wait(&_c->cond, &_c->lock, (long)(deadline - now));
        }
        int done = _c->reader_done;
        mapepire_mutex_unlock(&_c->lock);
        if (!done) mapepire_ws_interrupt(_c->ws);
        mapepire_thread_join(&_c->reader);
    }
    if (_c->ws) mapepire_ws_close(_c->ws);
    /* nobody can be waiting any more: free unclaimed replies */
    for (waiter *w = _c->waiters; w; w = w->next) mpjson_free(w->resp);
    mapepire_cond_destroy(&_c->cond);
    mapepire_mutex_destroy(&_c->lock);
    free(_c->technique);
    free(_c->application);
    free(_c);
}

int mapepire_client_http_get(mapepire_client *_c, const char *_path, mapepire_buf *_out, mapepire_error *_e)
{
    char err[512] = "";
    int rc = mapepire_ws_http_get(_c->ws, _path, _out, err, sizeof err);
    if (rc) {
        mapepire_error_set(_e, kind_from_ws(rc), "HY000", "Could not download a LOB value: %s", err);
        return -1;
    }
    return 0;
}

int mapepire_client_is_dead(mapepire_client *_c)
{
    if (!_c) return 1;
    mapepire_mutex_lock(&_c->lock);
    int d = _c->dead;
    mapepire_mutex_unlock(&_c->lock);
    return d;
}

const char *mapepire_client_job(mapepire_client *_c) { return _c->job; }
const char *mapepire_client_version(mapepire_client *_c) { return _c->version; }

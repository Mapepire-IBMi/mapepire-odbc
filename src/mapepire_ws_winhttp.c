/*
 * WinHTTP WebSocket backend (Windows only).
 *
 * WinHTTP is used in asynchronous mode.  The synchronous WebSocket API can
 * lose a wake-up when a send and a receive start at the same moment on two
 * threads (a receive then only returns once something else is sent), and the
 * client keeps a receive outstanding on its reader thread at all times.  Each
 * asynchronous operation below is still used synchronously by its caller: it
 * is started, then the caller waits on an event that the status callback sets
 * when WinHTTP reports completion.
 */
#ifdef _WIN32

#include "mapepire_ws.h"

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdlib.h>
#include <string.h>

typedef struct winhttp_ws winhttp_ws;

/* One outstanding operation and its completion state. */
typedef struct {
    HANDLE ev;      /* manual reset; set by the callback on completion */
    DWORD  err;     /* 0 on success */
    DWORD  bytes;
    int    type;    /* WINHTTP_WEB_SOCKET_BUFFER_TYPE for receives */
} op_t;

/* Context of one WinHTTP handle (a request or the WebSocket). */
typedef struct {
    winhttp_ws   *w;
    int           is_ws;
    op_t          op;        /* request ops; close/shutdown on the WebSocket */
    op_t          send;      /* WebSocket send */
    op_t          recv;      /* WebSocket receive */
    HANDLE        closed_ev; /* set on WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING */
    volatile LONG closed;
    volatile LONG pin_closed;
    char          pin_error[256];
} hctx;

struct winhttp_ws {
    mapepire_ws      base;
    HINTERNET  session;
    HINTERNET  connect;
    HINTERNET  socket;
    int        socket_opened;
    hctx       wsctx;
    wchar_t   *auth_header;   /* L"Authorization: Basic ..." */
    int        secure;
    DWORD      sec_flags;     /* security flags applied to every request */
    char       pin[65];       /* pinned fingerprint or "" */
    DWORD      io_timeout_ms; /* INFINITE when unset */
    DWORD      hs_timeout_ms; /* bound for handshake and download steps */
    mapepire_mutex   send_lock;     /* one outstanding send per WebSocket */
};

static wchar_t *to_wide(const char *_s)
{
    size_t n = mapepire_utf8_to_utf16(_s, strlen(_s), NULL, 0);
    wchar_t *w = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (w) mapepire_utf8_to_utf16(_s, strlen(_s), (uint16_t *)w, n + 1);
    return w;
}

static void win_error(char *_err, size_t _errlen, const char *_what, DWORD _code)
{
    char msg[256] = "";
    HMODULE mod = GetModuleHandleW(L"winhttp.dll");
    DWORD fl = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    if (_code >= 12000 && _code <= 12200 && mod) fl |= FORMAT_MESSAGE_FROM_HMODULE;
    if (!FormatMessageA(fl, mod, _code, 0, msg, sizeof msg, NULL)) msg[0] = '\0';
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' ' || msg[n - 1] == '.')) msg[--n] = '\0';
    mapepire_snprintf(_err, _errlen, "%s failed (WinHTTP error %lu%s%s)", _what, (unsigned long)_code, n ? ": " : "", msg);
}

static int map_error(DWORD _code)
{
    switch (_code) {
    case ERROR_WINHTTP_TIMEOUT: return MAPEPIRE_WS_ETIMEOUT;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
        return MAPEPIRE_WS_ETLS;
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return MAPEPIRE_WS_ECONNECT;
    default: return MAPEPIRE_WS_EIO;
    }
}

/* ---- handle contexts and operations ---------------------------------------------- */

static int ctx_init(hctx *_c, winhttp_ws *_w, int _is_ws)
{
    memset(_c, 0, sizeof *_c);
    _c->w = _w;
    _c->is_ws = _is_ws;
    _c->op.ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    _c->send.ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    _c->recv.ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    _c->closed_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    return _c->op.ev && _c->send.ev && _c->recv.ev && _c->closed_ev ? 0 : -1;
}

static void ctx_free(hctx *_c)
{
    if (_c->op.ev) CloseHandle(_c->op.ev);
    if (_c->send.ev) CloseHandle(_c->send.ev);
    if (_c->recv.ev) CloseHandle(_c->recv.ev);
    if (_c->closed_ev) CloseHandle(_c->closed_ev);
    memset(_c, 0, sizeof *_c);
}

static void op_begin(op_t *_o)
{
    _o->err = 0;
    _o->bytes = 0;
    _o->type = 0;
    ResetEvent(_o->ev);
}

static void op_done(op_t *_o, DWORD _err)
{
    _o->err = _err;
    SetEvent(_o->ev);
}

/* Wait for an operation started with op_begin; returns its WinHTTP error code. */
static DWORD op_wait(hctx *_c, op_t *_o, DWORD _timeout_ms)
{
    if (WaitForSingleObject(_o->ev, _timeout_ms) != WAIT_OBJECT_0) return ERROR_WINHTTP_TIMEOUT;
    if (_c->pin_closed && !_o->err) return ERROR_WINHTTP_OPERATION_CANCELLED;
    return _o->err;
}

/* Close a request handle and wait until WinHTTP has stopped using its context. */
static void close_request(HINTERNET _req, hctx *_c)
{
    if (!_c->pin_closed && !_c->closed) WinHttpCloseHandle(_req);
    WaitForSingleObject(_c->closed_ev, 10000);
}

/* Compare the server certificate's SHA-256 against the pinned value. */
static int check_pin(winhttp_ws *_w, HINTERNET _req, char *_err, size_t _errlen)
{
    if (!_w->secure || !_w->pin[0]) return MAPEPIRE_WS_OK;
    PCCERT_CONTEXT cert = NULL;
    DWORD sz = sizeof cert;
    if (!WinHttpQueryOption(_req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &cert, &sz) || !cert) {
        win_error(_err, _errlen, "Reading the server certificate", GetLastError());
        return MAPEPIRE_WS_ETLS;
    }
    BYTE digest[32];
    DWORD dlen = sizeof digest;
    BOOL ok = CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, NULL, cert->pbCertEncoded,
                                    cert->cbCertEncoded, digest, &dlen);
    CertFreeCertificateContext(cert);
    if (!ok || dlen != 32) {
        win_error(_err, _errlen, "Hashing the server certificate", GetLastError());
        return MAPEPIRE_WS_ETLS;
    }
    char hex[65];
    mapepire_ws_hex32(digest, hex);
    if (strcmp(hex, _w->pin)) {
        mapepire_snprintf(_err, _errlen,
                    "The server certificate SHA-256 fingerprint %s does not match the pinned CertSHA256 value", hex);
        return MAPEPIRE_WS_ETLS;
    }
    return MAPEPIRE_WS_OK;
}

static void CALLBACK status_cb(HINTERNET _h, DWORD_PTR _ctx, DWORD _status, LPVOID _info, DWORD _infolen)
{
    hctx *c = (hctx *)_ctx;
    if (!c) return; /* session / connect handles carry no context */
    switch (_status) {
    case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST:
        /*
         * Fires after the TLS handshake and before any request bytes (including
         * the Authorization header) are written, on every connection used.  On
         * a pin mismatch the request handle is closed, which cancels the send.
         */
        if (c->w->secure && c->w->pin[0] && !c->pin_closed &&
            check_pin(c->w, _h, c->pin_error, sizeof c->pin_error) != MAPEPIRE_WS_OK) {
            InterlockedExchange(&c->pin_closed, 1);
            WinHttpCloseHandle(_h);
        }
        break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        op_done(&c->op, 0);
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        c->op.bytes = _info ? *(DWORD *)_info : 0;
        op_done(&c->op, 0);
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        if (c->is_ws) {
            WINHTTP_WEB_SOCKET_STATUS *st = (WINHTTP_WEB_SOCKET_STATUS *)_info;
            c->recv.bytes = st ? st->dwBytesTransferred : 0;
            c->recv.type = st ? (int)st->eBufferType : 0;
            op_done(&c->recv, 0);
        } else {
            c->op.bytes = _infolen;
            op_done(&c->op, 0);
        }
        break;
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
        if (c->is_ws) op_done(&c->send, 0);
        break;
    case WINHTTP_CALLBACK_STATUS_CLOSE_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_SHUTDOWN_COMPLETE:
        op_done(&c->op, 0);
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        if (c->is_ws) {
            WINHTTP_WEB_SOCKET_ASYNC_RESULT *r = (WINHTTP_WEB_SOCKET_ASYNC_RESULT *)_info;
            DWORD e = r && r->AsyncResult.dwError ? r->AsyncResult.dwError : ERROR_WINHTTP_INTERNAL_ERROR;
            if (r && r->Operation == WINHTTP_WEB_SOCKET_SEND_OPERATION) op_done(&c->send, e);
            else if (r && r->Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION) op_done(&c->recv, e);
            else op_done(&c->op, e);
        } else {
            WINHTTP_ASYNC_RESULT *r = (WINHTTP_ASYNC_RESULT *)_info;
            op_done(&c->op, r && r->dwError ? r->dwError : ERROR_WINHTTP_INTERNAL_ERROR);
        }
        break;
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        /* the last notification for this handle: fail anything still waiting */
        InterlockedExchange(&c->closed, 1);
        if (WaitForSingleObject(c->op.ev, 0) != WAIT_OBJECT_0) op_done(&c->op, ERROR_WINHTTP_OPERATION_CANCELLED);
        if (WaitForSingleObject(c->send.ev, 0) != WAIT_OBJECT_0) op_done(&c->send, ERROR_WINHTTP_OPERATION_CANCELLED);
        if (WaitForSingleObject(c->recv.ev, 0) != WAIT_OBJECT_0) op_done(&c->recv, ERROR_WINHTTP_OPERATION_CANCELLED);
        SetEvent(c->closed_ev);
        break;
    default:
        break;
    }
}

/* ---- HTTP requests (handshake and LOB downloads) ----------------------------------- */

static HINTERNET open_request(winhttp_ws *_w, const char *_path, int _with_auth, char *_err, size_t _errlen)
{
    wchar_t *wpath = to_wide(_path && *_path ? _path : "/");
    HINTERNET req = WinHttpOpenRequest(_w->connect, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, _w->secure ? WINHTTP_FLAG_SECURE : 0);
    free(wpath);
    if (!req) { win_error(_err, _errlen, "WinHttpOpenRequest", GetLastError()); return NULL; }
    if (_w->sec_flags) {
        DWORD f = _w->sec_flags;
        WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &f, sizeof f);
    }
    /* Never let WinHTTP follow redirects on its own. */
    DWORD pol = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &pol, sizeof pol);
    if (_with_auth && !WinHttpAddRequestHeaders(req, _w->auth_header, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)) {
        win_error(_err, _errlen, "WinHttpAddRequestHeaders", GetLastError());
        WinHttpCloseHandle(req);
        return NULL;
    }
    return req;
}

static int request_failed(winhttp_ws *_w, hctx *_c, DWORD _e, const char *_what, char *_err, size_t _errlen)
{
    (void)_w;
    if (_c->pin_closed) {
        mapepire_snprintf(_err, _errlen, "%s", _c->pin_error);
        return MAPEPIRE_WS_ETLS;
    }
    win_error(_err, _errlen, _what, _e);
    int rc = map_error(_e);
    if (rc == MAPEPIRE_WS_ETLS)
        mapepire_snprintf(_err + strlen(_err), _errlen - strlen(_err),
                    ". If the server uses a self-signed certificate, set CertSHA256=<fingerprint> "
                    "or TrustServerCertificate=yes");
    return rc;
}

/* Send the request and wait for the response headers. */
static int send_and_receive(winhttp_ws *_w, HINTERNET _req, hctx *_c, DWORD *_status, char *_err, size_t _errlen)
{
    op_begin(&_c->op);
    if (!WinHttpSendRequest(_req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, (DWORD_PTR)_c))
        return request_failed(_w, _c, GetLastError(), "Connecting to the Mapepire server", _err, _errlen);
    DWORD e = op_wait(_c, &_c->op, _w->hs_timeout_ms);
    if (e) return request_failed(_w, _c, e, "Connecting to the Mapepire server", _err, _errlen);

    op_begin(&_c->op);
    if (!WinHttpReceiveResponse(_req, NULL))
        return request_failed(_w, _c, GetLastError(), "Receiving the server response", _err, _errlen);
    e = op_wait(_c, &_c->op, _w->hs_timeout_ms);
    if (e) return request_failed(_w, _c, e, "Receiving the server response", _err, _errlen);

    int rc = check_pin(_w, _req, _err, _errlen); /* defence in depth; the callback already checked */
    if (rc) return rc;
    DWORD sz = sizeof *_status;
    if (!WinHttpQueryHeaders(_req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             _status, &sz, WINHTTP_NO_HEADER_INDEX)) {
        win_error(_err, _errlen, "Reading the HTTP status", GetLastError());
        return MAPEPIRE_WS_EPROTO;
    }
    return MAPEPIRE_WS_OK;
}

static int w_http_get(mapepire_ws *_ws, const char *_path, mapepire_buf *_out, char *_err, size_t _errlen)
{
    winhttp_ws *w = (winhttp_ws *)_ws;
    hctx c;
    if (ctx_init(&c, w, 0)) { ctx_free(&c); mapepire_snprintf(_err, _errlen, "CreateEvent failed"); return MAPEPIRE_WS_EIO; }
    HINTERNET req = open_request(w, _path, 1, _err, _errlen);
    if (!req) { ctx_free(&c); return MAPEPIRE_WS_EIO; }
    /* Never return this connection to WinHTTP's pool: a server that closes it
     * after replying (as the mock does on 404) would otherwise break the next
     * WebSocket upgrade, which may be sent over the stale pooled connection. */
    DWORD nokeep = WINHTTP_DISABLE_KEEP_ALIVE;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &nokeep, sizeof nokeep);
    DWORD status = 0;
    int rc = send_and_receive(w, req, &c, &status, _err, _errlen);
    if (!rc && status != 200) {
        mapepire_snprintf(_err, _errlen, "HTTP GET %s returned status %lu", _path, (unsigned long)status);
        rc = MAPEPIRE_WS_EPROTO;
    }
    while (!rc) {
        op_begin(&c.op);
        if (!WinHttpQueryDataAvailable(req, NULL)) {
            rc = request_failed(w, &c, GetLastError(), "Reading the HTTP body", _err, _errlen);
            break;
        }
        DWORD e = op_wait(&c, &c.op, w->hs_timeout_ms);
        if (e) { rc = request_failed(w, &c, e, "Reading the HTTP body", _err, _errlen); break; }
        DWORD avail = c.op.bytes;
        if (!avail) break;
        if (mapepire_buf_reserve(_out, avail)) { mapepire_snprintf(_err, _errlen, "Out of memory"); rc = MAPEPIRE_WS_EIO; break; }
        op_begin(&c.op);
        if (!WinHttpReadData(req, _out->data + _out->len, avail, NULL)) {
            rc = request_failed(w, &c, GetLastError(), "Reading the HTTP body", _err, _errlen);
            break;
        }
        e = op_wait(&c, &c.op, w->hs_timeout_ms);
        if (e) { rc = request_failed(w, &c, e, "Reading the HTTP body", _err, _errlen); break; }
        _out->len += c.op.bytes;
        _out->data[_out->len] = '\0';
        if (!c.op.bytes) break;
    }
    close_request(req, &c);
    ctx_free(&c);
    return rc;
}

/* ---- WebSocket operations --------------------------------------------------------- */

static int w_send(mapepire_ws *_ws, const char *_data, size_t _len, char *_err, size_t _errlen)
{
    winhttp_ws *w = (winhttp_ws *)_ws;
    if (_len > 0xFFFFFFFFu) { mapepire_snprintf(_err, _errlen, "Message too large"); return MAPEPIRE_WS_EIO; }
    mapepire_mutex_lock(&w->send_lock);
    DWORD rc = ERROR_WINHTTP_OPERATION_CANCELLED;
    HINTERNET sock = w->socket;
    if (sock && !w->wsctx.closed) {
        op_begin(&w->wsctx.send);
        rc = WinHttpWebSocketSend(sock, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)_data, (DWORD)_len);
        if (rc == NO_ERROR) rc = op_wait(&w->wsctx, &w->wsctx.send, w->io_timeout_ms);
    }
    mapepire_mutex_unlock(&w->send_lock);
    if (rc != NO_ERROR) {
        win_error(_err, _errlen, "Sending a WebSocket message", rc);
        return map_error(rc) == MAPEPIRE_WS_ETIMEOUT ? MAPEPIRE_WS_ETIMEOUT : MAPEPIRE_WS_EIO;
    }
    return MAPEPIRE_WS_OK;
}

static int w_recv(mapepire_ws *_ws, mapepire_buf *_out, char *_err, size_t _errlen)
{
    winhttp_ws *w = (winhttp_ws *)_ws;
    hctx *c = &w->wsctx;
    for (;;) {
        if (mapepire_buf_reserve(_out, 65536)) { mapepire_snprintf(_err, _errlen, "Out of memory"); return MAPEPIRE_WS_EIO; }
        HINTERNET sock = w->socket;
        if (!sock || c->closed) { mapepire_snprintf(_err, _errlen, "The connection was closed"); return MAPEPIRE_WS_ECLOSED; }
        op_begin(&c->recv);
        DWORD rc = WinHttpWebSocketReceive(sock, _out->data + _out->len, (DWORD)(_out->cap - _out->len - 1), NULL, NULL);
        if (rc == NO_ERROR) rc = op_wait(c, &c->recv, INFINITE);
        if (rc != NO_ERROR) {
            win_error(_err, _errlen, "Receiving a WebSocket message", rc);
            return MAPEPIRE_WS_EIO;
        }
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type = (WINHTTP_WEB_SOCKET_BUFFER_TYPE)c->recv.type;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT status = 0;
            BYTE reason[128];
            DWORD rlen = 0;
            WinHttpWebSocketQueryCloseStatus(sock, &status, reason, sizeof reason - 1, &rlen);
            reason[rlen] = 0;
            mapepire_snprintf(_err, _errlen, "The server closed the connection (code %u%s%s)", (unsigned)status,
                        rlen ? ": " : "", (char *)reason);
            return MAPEPIRE_WS_ECLOSED;
        }
        _out->len += c->recv.bytes;
        _out->data[_out->len] = '\0';
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            return MAPEPIRE_WS_OK;
        /* *_FRAGMENT_BUFFER_TYPE: keep reading */
    }
}

static void w_interrupt(mapepire_ws *_ws)
{
    winhttp_ws *w = (winhttp_ws *)_ws;
    /* Closing the handle completes a receive pending on another thread with an error. */
    HINTERNET sock = (HINTERNET)InterlockedExchangePointer((PVOID volatile *)&w->socket, NULL);
    if (sock) WinHttpCloseHandle(sock);
}

static void w_close(mapepire_ws *_ws)
{
    winhttp_ws *w = (winhttp_ws *)_ws;
    if (w->socket_opened) {
        HINTERNET sock = (HINTERNET)InterlockedExchangePointer((PVOID volatile *)&w->socket, NULL);
        if (sock) {
            if (!w->wsctx.closed) {
                op_begin(&w->wsctx.op);
                if (WinHttpWebSocketClose(sock, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0) == NO_ERROR)
                    op_wait(&w->wsctx, &w->wsctx.op, 2000);
            }
            WinHttpCloseHandle(sock);
        }
        /* the context must outlive the handle's last callback */
        WaitForSingleObject(w->wsctx.closed_ev, 10000);
    }
    if (w->connect) WinHttpCloseHandle(w->connect);
    if (w->session) WinHttpCloseHandle(w->session);
    ctx_free(&w->wsctx);
    if (w->auth_header) {
        SecureZeroMemory(w->auth_header, wcslen(w->auth_header) * sizeof(wchar_t));
        free(w->auth_header);
    }
    mapepire_mutex_destroy(&w->send_lock);
    free(w);
}

static const mapepire_ws_vtbl winhttp_vtbl = {w_send, w_recv, w_http_get, w_interrupt, w_close};

int mapepire_ws_open_winhttp(mapepire_ws **_out, const mapepire_ws_params *_p, char *_err, size_t _errlen)
{
    *_out = NULL;
    winhttp_ws *w = (winhttp_ws *)calloc(1, sizeof *w);
    if (!w) return MAPEPIRE_WS_EIO;
    w->base.vt = &winhttp_vtbl;
    w->secure = _p->secure;
    mapepire_mutex_init(&w->send_lock);
    int rc = MAPEPIRE_WS_ECONNECT;
    HINTERNET req = NULL;
    hctx rctx;
    int rctx_ready = 0;
    memset(&rctx, 0, sizeof rctx);

    int ct = _p->connect_timeout > 0 ? _p->connect_timeout * 1000 : 60000;
    w->hs_timeout_ms = (DWORD)(ct * 2 + 5000);
    w->io_timeout_ms = _p->io_timeout > 0 ? (DWORD)_p->io_timeout * 1000 : INFINITE;
    if (ctx_init(&w->wsctx, w, 1)) { mapepire_snprintf(_err, _errlen, "CreateEvent failed"); goto fail; }

    if (_p->cert_sha256 && *_p->cert_sha256) {
        if (mapepire_ws_normalize_fingerprint(_p->cert_sha256, w->pin)) {
            mapepire_snprintf(_err, _errlen, "CertSHA256 must be 64 hexadecimal digits (colons allowed)");
            rc = MAPEPIRE_WS_ETLS;
            goto fail;
        }
    }
    /* Chain validation is replaced by the pin when one is given. */
    if (_p->secure && (!_p->verify_cert || w->pin[0]))
        w->sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                       SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    {
        char *auth = mapepire_ws_basic_auth(_p->user, _p->password);
        if (!auth) goto fail;
        mapepire_buf h;
        mapepire_buf_init(&h);
        mapepire_buf_append_str(&h, "Authorization: ");
        mapepire_buf_append_str(&h, auth);
        SecureZeroMemory(auth, strlen(auth));
        free(auth);
        w->auth_header = to_wide(h.data);
        SecureZeroMemory(h.data, h.len);
        mapepire_buf_free(&h);
        if (!w->auth_header) goto fail;
    }

    w->session = WinHttpOpen(L"mapepire-odbc", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!w->session) { win_error(_err, _errlen, "WinHttpOpen", GetLastError()); goto fail; }
    /* every handle created from the session inherits the callback */
    if (WinHttpSetStatusCallback(w->session, status_cb,
                                 WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_SEND_REQUEST |
                                     WINHTTP_CALLBACK_FLAG_HANDLES,
                                 0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        win_error(_err, _errlen, "WinHttpSetStatusCallback", GetLastError());
        goto fail;
    }
    {
        int io = _p->io_timeout > 0 ? _p->io_timeout * 1000 : 0; /* 0 = infinite */
        /* the receive timeout covers the handshake and LOB downloads */
        WinHttpSetTimeouts(w->session, ct, ct, io, io ? io : ct);
    }
    {
        wchar_t *whost = to_wide(_p->host);
        w->connect = WinHttpConnect(w->session, whost, (INTERNET_PORT)_p->port, 0);
        free(whost);
    }
    if (!w->connect) { win_error(_err, _errlen, "WinHttpConnect", GetLastError()); goto fail; }

    if (ctx_init(&rctx, w, 0)) { mapepire_snprintf(_err, _errlen, "CreateEvent failed"); goto fail; }
    rctx_ready = 1;
    req = open_request(w, _p->path, 1, _err, _errlen);
    if (!req) { rc = MAPEPIRE_WS_EIO; goto fail; }
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        win_error(_err, _errlen, "Enabling the WebSocket upgrade", GetLastError());
        rc = MAPEPIRE_WS_EIO;
        goto fail;
    }
    DWORD status = 0;
    rc = send_and_receive(w, req, &rctx, &status, _err, _errlen);
    if (rc) goto fail;
    if (status != 101) {
        if (status == 401 || status == 403 || status == 404) {
            mapepire_snprintf(_err, _errlen,
                        "The server refused the WebSocket connection (HTTP %lu). Check the user ID, password "
                        "and that the server is a Mapepire server",
                        (unsigned long)status);
            rc = MAPEPIRE_WS_EAUTH;
        } else {
            mapepire_snprintf(_err, _errlen, "The server refused the WebSocket upgrade (HTTP %lu)", (unsigned long)status);
            rc = MAPEPIRE_WS_EPROTO;
        }
        goto fail;
    }
    w->socket = WinHttpWebSocketCompleteUpgrade(req, (DWORD_PTR)&w->wsctx);
    if (!w->socket) {
        win_error(_err, _errlen, "Completing the WebSocket upgrade", GetLastError());
        rc = MAPEPIRE_WS_EPROTO;
        goto fail;
    }
    w->socket_opened = 1;
    close_request(req, &rctx);
    ctx_free(&rctx);
    *_out = &w->base;
    return MAPEPIRE_WS_OK;

fail:
    if (req) close_request(req, &rctx);
    if (rctx_ready) ctx_free(&rctx);
    w_close(&w->base);
    return rc ? rc : MAPEPIRE_WS_ECONNECT;
}

#else
typedef int mapepire_ws_winhttp_unused;
#endif

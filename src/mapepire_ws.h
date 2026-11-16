/*
 * WebSocket client transport used to talk to a Mapepire server.
 *
 * Two backends implement this interface:
 *   - winhttp : Windows only.  Uses the WinHTTP WebSocket API, so TLS is
 *               handled by Schannel and the Windows certificate store.
 *   - socket  : Portable RFC 6455 implementation over BSD sockets / Winsock.
 *               TLS requires building with OpenSSL (MAPEPIRE_HAVE_OPENSSL).
 */
#ifndef MAPEPIRE_WS_H
#define MAPEPIRE_WS_H

#include "mapepire_util.h"

enum {
    MAPEPIRE_WS_OK = 0,
    MAPEPIRE_WS_ECONNECT = -1,  /* could not reach the server */
    MAPEPIRE_WS_EAUTH = -2,     /* upgrade refused (401/403/404): bad credentials or path */
    MAPEPIRE_WS_ETLS = -3,      /* TLS / certificate failure */
    MAPEPIRE_WS_EIO = -4,       /* read / write failure after connecting */
    MAPEPIRE_WS_ECLOSED = -5,   /* server closed the WebSocket */
    MAPEPIRE_WS_ETIMEOUT = -6,
    MAPEPIRE_WS_EPROTO = -7,    /* malformed handshake or frame */
    MAPEPIRE_WS_EUNSUPPORTED = -8
};

typedef enum { MAPEPIRE_WS_BACKEND_DEFAULT = 0, MAPEPIRE_WS_BACKEND_WINHTTP, MAPEPIRE_WS_BACKEND_SOCKET } mapepire_ws_backend;

typedef struct mapepire_ws_params {
    const char *host;
    int         port;
    const char *path;            /* e.g. "/db/" */
    int         secure;          /* wss:// when non-zero */
    int         verify_cert;     /* validate the certificate chain and host name */
    const char *ca_file;         /* PEM bundle (socket/OpenSSL backend only) */
    const char *cert_sha256;     /* optional pinned SHA-256 fingerprint (hex, ':' allowed) */
    const char *user;
    const char *password;
    int         connect_timeout; /* seconds, 0 = OS default */
    int         io_timeout;      /* seconds for sends and HTTP downloads, 0 = wait forever
                                  * (receives always block: the reader thread idles there) */
    mapepire_ws_backend backend;
} mapepire_ws_params;

typedef struct mapepire_ws mapepire_ws;

/*
 * Threading contract: one thread may block in mapepire_ws_recv while any number of
 * other threads call mapepire_ws_send (sends are serialised internally) and
 * mapepire_ws_http_get.  mapepire_ws_interrupt may be called from any thread to make a
 * blocked mapepire_ws_recv return; mapepire_ws_close must only be called once no thread
 * is inside any other mapepire_ws_* function.
 */

/* Open a WebSocket.  On failure returns a negative MAPEPIRE_WS_* code and fills err. */
int  mapepire_ws_open(mapepire_ws **_out, const mapepire_ws_params *_p, char *_err, size_t _errlen);
/* Send one text message. */
int  mapepire_ws_send(mapepire_ws *_ws, const char *_data, size_t _len, char *_err, size_t _errlen);
/* Receive one complete (reassembled) message into out (cleared first). */
int  mapepire_ws_recv(mapepire_ws *_ws, mapepire_buf *_out, char *_err, size_t _errlen);
/* HTTP GET a path on the same server with the same credentials (used for
 * BLOB values the server returns as {"blob_url": ...}). */
int  mapepire_ws_http_get(mapepire_ws *_ws, const char *_path, mapepire_buf *_out, char *_err, size_t _errlen);
/* Abort the connection so that a blocked mapepire_ws_recv returns an error. */
void mapepire_ws_interrupt(mapepire_ws *_ws);
/* Send a close frame (best effort) and free everything. */
void mapepire_ws_close(mapepire_ws *_ws);

/* ---- backend plumbing (internal) --------------------------------------- */

typedef struct mapepire_ws_vtbl {
    int  (*send)(mapepire_ws *_ws, const char *_data, size_t _len, char *_err, size_t _errlen);
    int  (*recv)(mapepire_ws *_ws, mapepire_buf *_out, char *_err, size_t _errlen);
    int  (*http_get)(mapepire_ws *_ws, const char *_path, mapepire_buf *_out, char *_err, size_t _errlen);
    void (*interrupt)(mapepire_ws *_ws);
    void (*close)(mapepire_ws *_ws);
} mapepire_ws_vtbl;

struct mapepire_ws {
    const mapepire_ws_vtbl *vt;
};

int mapepire_ws_open_winhttp(mapepire_ws **_out, const mapepire_ws_params *_p, char *_err, size_t _errlen);
int mapepire_ws_open_socket(mapepire_ws **_out, const mapepire_ws_params *_p, char *_err, size_t _errlen);

/* Build "Basic <base64(user:password)>" (malloc'd). */
char *mapepire_ws_basic_auth(const char *_user, const char *_password);
/* Normalise a hex fingerprint (strip ':' / spaces, lowercase) into out[65]; 0 on success. */
int   mapepire_ws_normalize_fingerprint(const char *_in, char _out[65]);
/* Hex-encode a 32-byte digest into out[65]. */
void  mapepire_ws_hex32(const unsigned char *_digest, char _out[65]);

#endif

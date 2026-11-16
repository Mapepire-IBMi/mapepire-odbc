/*
 * Portable RFC 6455 WebSocket client over BSD sockets / Winsock.
 * TLS is provided by OpenSSL when built with MAPEPIRE_HAVE_OPENSSL.
 */
#include "mapepire_ws.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#define sock_errno() WSAGetLastError()
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#define sock_errno() errno
#endif

#ifdef MAPEPIRE_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* A peer reset must surface as an error, never as SIGPIPE killing the application. */
#if defined(MSG_NOSIGNAL)
#define MAPEPIRE_SEND_FLAGS MSG_NOSIGNAL
#else
#define MAPEPIRE_SEND_FLAGS 0
#endif

#ifdef MAPEPIRE_HAVE_OPENSSL
#if OPENSSL_VERSION_NUMBER < 0x30000000L
#define SSL_get1_peer_certificate SSL_get_peer_certificate
#endif
#if !defined(_WIN32) && !defined(SO_NOSIGPIPE)
#include <signal.h>
/* OpenSSL writes with write(2); block SIGPIPE for the calling thread meanwhile. */
#define MAPEPIRE_SIGPIPE_GUARD 1
#endif
#endif

#ifdef MAPEPIRE_HAVE_OPENSSL
static int is_ip_literal(const char *_h)
{
    unsigned char buf[16];
    return inet_pton(AF_INET, _h, buf) == 1 || inet_pton(AF_INET6, _h, buf) == 1;
}
#endif

typedef struct {
    sock_t fd;
#ifdef MAPEPIRE_HAVE_OPENSSL
    SSL_CTX *ctx;
    SSL     *ssl;
    /* OpenSSL forbids concurrent SSL_read/SSL_write on one SSL object, so with
     * TLS the socket is non-blocking and each SSL call runs under io_lock;
     * threads wait for readiness with poll() outside the lock. */
    mapepire_mutex io_lock;
    int      io_lock_init;
#endif
    int      send_timeout_ms;  /* -1 = forever */
    int      recv_timeout_ms;  /* -1 = forever */
    mapepire_buf   in;       /* buffered, not yet consumed input (reader thread only) */
    size_t   in_pos;
} conn_t;

typedef struct {
    mapepire_ws        base;
    conn_t       c;
    mapepire_ws_params p;        /* copies of strings below are owned */
    char        *host;
    char        *path;
    char        *ca_file;
    char        *auth;     /* "Basic ..." */
    char         pin[65];
    mapepire_mutex     send_lock; /* frames from different threads must not interleave */
} sock_ws;

/* ---- low level connection ------------------------------------------------ */

static void sock_err(char *_err, size_t _errlen, const char *_what)
{
    mapepire_snprintf(_err, _errlen, "%s failed (socket error %d)", _what, sock_errno());
}

static int ensure_winsock(void)
{
#ifdef _WIN32
    static volatile LONG done = 0;
    if (!done) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d)) return -1;
        InterlockedExchange(&done, 1);
    }
#endif
    return 0;
}

static void set_timeouts(sock_t _fd, int _send_seconds, int _recv_seconds)
{
#ifdef _WIN32
    DWORD snd = (DWORD)(_send_seconds > 0 ? _send_seconds : 0) * 1000;
    DWORD rcv = (DWORD)(_recv_seconds > 0 ? _recv_seconds : 0) * 1000;
    if (snd) setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&snd, sizeof snd);
    if (rcv) setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcv, sizeof rcv);
#else
    struct timeval snd = {_send_seconds > 0 ? _send_seconds : 0, 0};
    struct timeval rcv = {_recv_seconds > 0 ? _recv_seconds : 0, 0};
    if (_send_seconds > 0) setsockopt(_fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
    if (_recv_seconds > 0) setsockopt(_fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
#endif
}

#ifdef MAPEPIRE_HAVE_OPENSSL
/* Wait until fd is readable (or writable); 1 = ready, 0 = timeout, -1 = error. */
static int wait_fd(sock_t _fd, int _for_write, int _ms)
{
#ifdef _WIN32
    WSAPOLLFD pfd;
    pfd.fd = _fd;
    pfd.events = _for_write ? POLLWRNORM : POLLRDNORM;
    pfd.revents = 0;
    int rc = WSAPoll(&pfd, 1, _ms);
#else
    struct pollfd pfd = {_fd, (short)(_for_write ? POLLOUT : POLLIN), 0};
    int rc;
    do rc = poll(&pfd, 1, _ms);
    while (rc < 0 && errno == EINTR);
#endif
    if (rc < 0) return -1;
    return rc > 0 ? 1 : 0;
}

static void set_nonblocking(sock_t _fd)
{
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(_fd, FIONBIO, &nb);
#else
    fcntl(_fd, F_SETFL, fcntl(_fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}
#endif

/* connect() with a timeout by temporarily switching to non-blocking mode */
static int connect_timeout(sock_t _fd, const struct sockaddr *_sa, socklen_t _len, int _seconds)
{
    if (_seconds <= 0) return connect(_fd, _sa, _len);
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(_fd, FIONBIO, &nb);
    int rc = connect(_fd, _sa, _len);
    if (rc && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wr, ex;
        FD_ZERO(&wr); FD_ZERO(&ex);
        FD_SET(_fd, &wr); FD_SET(_fd, &ex);
        struct timeval tv = {_seconds, 0};
        rc = select(0, NULL, &wr, &ex, &tv);
        if (rc == 1 && FD_ISSET(_fd, &wr)) rc = 0;
        else { if (rc == 0) WSASetLastError(WSAETIMEDOUT); rc = -1; }
    }
    nb = 0;
    ioctlsocket(_fd, FIONBIO, &nb);
    return rc;
#else
    int fl = fcntl(_fd, F_GETFL, 0);
    fcntl(_fd, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(_fd, _sa, _len);
    if (rc && errno == EINPROGRESS) {
        struct pollfd pfd = {_fd, POLLOUT, 0};
        rc = poll(&pfd, 1, _seconds * 1000);
        if (rc == 1) {
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            getsockopt(_fd, SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr) { errno = soerr; rc = -1; } else rc = 0;
        } else {
            if (rc == 0) errno = ETIMEDOUT;
            rc = -1;
        }
    }
    fcntl(_fd, F_SETFL, fl);
    return rc;
#endif
}

#ifdef MAPEPIRE_HAVE_OPENSSL
static void ssl_error(char *_err, size_t _errlen, const char *_what)
{
    unsigned long e = ERR_get_error();
    char buf[256] = "";
    if (e) ERR_error_string_n(e, buf, sizeof buf);
    mapepire_snprintf(_err, _errlen, "%s failed%s%s", _what, buf[0] ? ": " : "", buf);
}
#endif

static void conn_close(conn_t *_c)
{
#ifdef MAPEPIRE_HAVE_OPENSSL
    if (_c->ssl) { SSL_shutdown(_c->ssl); SSL_free(_c->ssl); _c->ssl = NULL; }
    if (_c->ctx) { SSL_CTX_free(_c->ctx); _c->ctx = NULL; }
    if (_c->io_lock_init) { mapepire_mutex_destroy(&_c->io_lock); _c->io_lock_init = 0; }
#endif
    if (_c->fd != BAD_SOCK) { sock_close(_c->fd); _c->fd = BAD_SOCK; }
    mapepire_buf_free(&_c->in);
    _c->in_pos = 0;
}

/* recv_timeouts: apply io_timeout to reads too (not for the WebSocket, whose
 * reader thread legitimately waits forever). */
static int conn_open(conn_t *_c, const mapepire_ws_params *_p, const char *_pin, int _recv_timeouts, char *_err, size_t _errlen)
{
    memset(_c, 0, sizeof *_c);
    _c->fd = BAD_SOCK;
    _c->send_timeout_ms = _p->io_timeout > 0 ? _p->io_timeout * 1000 : -1;
    _c->recv_timeout_ms = _recv_timeouts && _p->io_timeout > 0 ? _p->io_timeout * 1000 : -1;
    mapepire_buf_init(&_c->in);
    if (ensure_winsock()) { mapepire_snprintf(_err, _errlen, "WSAStartup failed"); return MAPEPIRE_WS_ECONNECT; }

    char port[16];
    snprintf(port, sizeof port, "%d", _p->port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(_p->host, port, &hints, &res);
    if (gai) {
        mapepire_snprintf(_err, _errlen, "Could not resolve host '%s': %s", _p->host, gai_strerror(gai));
        return MAPEPIRE_WS_ECONNECT;
    }
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock_t fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == BAD_SOCK) continue;
        if (connect_timeout(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen, _p->connect_timeout) == 0) {
            _c->fd = fd;
            break;
        }
        sock_err(_err, _errlen, "connect");
        sock_close(fd);
    }
    freeaddrinfo(res);
    if (_c->fd == BAD_SOCK) {
        char detail[160];
        mapepire_snprintf(detail, sizeof detail, "%s", _err && _err[0] ? _err : "connect failed");
        mapepire_snprintf(_err, _errlen, "Could not connect to %s:%d (%s)", _p->host, _p->port, detail);
        return MAPEPIRE_WS_ECONNECT;
    }
    int one = 1;
    setsockopt(_c->fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(_c->fd, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&one, sizeof one);
#endif
    set_timeouts(_c->fd, _p->io_timeout, _recv_timeouts ? _p->io_timeout : 0);

    if (!_p->secure) return MAPEPIRE_WS_OK;
#ifdef MAPEPIRE_HAVE_OPENSSL
    _c->ctx = SSL_CTX_new(TLS_client_method());
    if (!_c->ctx) { ssl_error(_err, _errlen, "SSL_CTX_new"); conn_close(_c); return MAPEPIRE_WS_ETLS; }
    SSL_CTX_set_min_proto_version(_c->ctx, TLS1_2_VERSION);
    SSL_CTX_set_mode(_c->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    int verify = _p->verify_cert && !(_pin && *_pin);
    if (verify) {
        SSL_CTX_set_verify(_c->ctx, SSL_VERIFY_PEER, NULL);
        int ok = (_p->ca_file && *_p->ca_file) ? SSL_CTX_load_verify_locations(_c->ctx, _p->ca_file, NULL)
                                             : SSL_CTX_set_default_verify_paths(_c->ctx);
        if (!ok) { ssl_error(_err, _errlen, "Loading CA certificates"); conn_close(_c); return MAPEPIRE_WS_ETLS; }
    } else {
        SSL_CTX_set_verify(_c->ctx, SSL_VERIFY_NONE, NULL);
    }
    _c->ssl = SSL_new(_c->ctx);
    if (!_c->ssl) { ssl_error(_err, _errlen, "SSL_new"); conn_close(_c); return MAPEPIRE_WS_ETLS; }
    int ip = is_ip_literal(_p->host);
    if (!ip) SSL_set_tlsext_host_name(_c->ssl, _p->host); /* SNI must not carry an IP address */
    if (verify) {
        if (ip) X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(_c->ssl), _p->host);
        else SSL_set1_host(_c->ssl, _p->host);
    }
    SSL_set_fd(_c->ssl, (int)_c->fd);
    if (SSL_connect(_c->ssl) != 1) {
        long vr = SSL_get_verify_result(_c->ssl);
        if (vr != X509_V_OK)
            mapepire_snprintf(_err, _errlen,
                        "TLS certificate verification failed: %s. If the server uses a self-signed certificate, "
                        "set CertSHA256=<fingerprint>, CAFile=<pem> or TrustServerCertificate=yes",
                        X509_verify_cert_error_string(vr));
        else
            ssl_error(_err, _errlen, "TLS handshake");
        conn_close(_c);
        return MAPEPIRE_WS_ETLS;
    }
    if (_pin && *_pin) {
        X509 *cert = SSL_get1_peer_certificate(_c->ssl);
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int mdlen = 0;
        int ok = cert && X509_digest(cert, EVP_sha256(), md, &mdlen) && mdlen == 32;
        if (cert) X509_free(cert);
        char hex[65] = "";
        if (ok) mapepire_ws_hex32(md, hex);
        if (!ok || strcmp(hex, _pin)) {
            mapepire_snprintf(_err, _errlen,
                        "The server certificate SHA-256 fingerprint %s does not match the pinned CertSHA256 value",
                        hex[0] ? hex : "(unavailable)");
            conn_close(_c);
            return MAPEPIRE_WS_ETLS;
        }
    }
    /* The handshake ran in blocking mode; from here on see conn_t.io_lock. */
    mapepire_mutex_init(&_c->io_lock);
    _c->io_lock_init = 1;
    set_nonblocking(_c->fd);
    return MAPEPIRE_WS_OK;
#else
    (void)_pin;
    mapepire_snprintf(_err, _errlen,
                "This build of the socket transport has no TLS support (built without OpenSSL). Use "
                "Secure=no for a ws:// server%s",
#ifdef _WIN32
                ", or Transport=winhttp"
#else
                ""
#endif
    );
    conn_close(_c);
    return MAPEPIRE_WS_EUNSUPPORTED;
#endif
}

static int conn_write(conn_t *_c, const void *_data, size_t _len, char *_err, size_t _errlen)
{
    const char *p = (const char *)_data;
    while (_len) {
        int chunk = _len > 1 << 30 ? 1 << 30 : (int)_len;
        int n;
#ifdef MAPEPIRE_HAVE_OPENSSL
        if (_c->ssl) {
#ifdef MAPEPIRE_SIGPIPE_GUARD
            sigset_t pipe_set, old_set;
            sigemptyset(&pipe_set);
            sigaddset(&pipe_set, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
#endif
            mapepire_mutex_lock(&_c->io_lock);
            n = SSL_write(_c->ssl, p, chunk);
            int se = n > 0 ? SSL_ERROR_NONE : SSL_get_error(_c->ssl, n);
#ifdef MAPEPIRE_SIGPIPE_GUARD
            {
                /* discard a SIGPIPE raised by this write before unblocking */
                struct timespec zero = {0, 0};
                while (sigtimedwait(&pipe_set, NULL, &zero) > 0) {}
                pthread_sigmask(SIG_SETMASK, &old_set, NULL);
            }
#endif
            if (se != SSL_ERROR_NONE && se != SSL_ERROR_WANT_READ && se != SSL_ERROR_WANT_WRITE)
                ssl_error(_err, _errlen, "TLS write");
            mapepire_mutex_unlock(&_c->io_lock);
            if (se == SSL_ERROR_WANT_WRITE || se == SSL_ERROR_WANT_READ) {
                /* WANT_READ (renegotiation / key update) is retried after a short
                 * wait: the reader thread may be the one consuming the input. */
                int r = se == SSL_ERROR_WANT_WRITE ? wait_fd(_c->fd, 1, _c->send_timeout_ms) : wait_fd(_c->fd, 0, 50);
                if (r < 0) { sock_err(_err, _errlen, "poll"); return MAPEPIRE_WS_EIO; }
                if (r == 0 && se == SSL_ERROR_WANT_WRITE) {
                    mapepire_snprintf(_err, _errlen, "Timed out writing to the server");
                    return MAPEPIRE_WS_ETIMEOUT;
                }
                continue;
            }
            if (n <= 0) return MAPEPIRE_WS_EIO;
        } else
#endif
        {
            n = (int)send(_c->fd, p, chunk, MAPEPIRE_SEND_FLAGS);
            if (n <= 0) {
                int e = sock_errno();
#ifdef _WIN32
                if (e == WSAETIMEDOUT) { mapepire_snprintf(_err, _errlen, "Timed out writing to the server"); return MAPEPIRE_WS_ETIMEOUT; }
#else
                if (e == EINTR) continue;
                if (e == EAGAIN || e == EWOULDBLOCK) { mapepire_snprintf(_err, _errlen, "Timed out writing to the server"); return MAPEPIRE_WS_ETIMEOUT; }
#endif
                sock_err(_err, _errlen, "send");
                return MAPEPIRE_WS_EIO;
            }
        }
        p += n;
        _len -= (size_t)n;
    }
    return MAPEPIRE_WS_OK;
}

/* Read more bytes into the input buffer. */
static int conn_fill(conn_t *_c, char *_err, size_t _errlen)
{
    if (_c->in_pos && _c->in_pos == _c->in.len) { mapepire_buf_clear(&_c->in); _c->in_pos = 0; }
    if (_c->in_pos > 65536 && _c->in_pos * 2 > _c->in.len) {
        memmove(_c->in.data, _c->in.data + _c->in_pos, _c->in.len - _c->in_pos);
        _c->in.len -= _c->in_pos;
        _c->in_pos = 0;
    }
    if (mapepire_buf_reserve(&_c->in, 65536)) { mapepire_snprintf(_err, _errlen, "Out of memory"); return MAPEPIRE_WS_EIO; }
    int n;
    for (;;) {
#ifdef MAPEPIRE_HAVE_OPENSSL
        if (_c->ssl) {
            mapepire_mutex_lock(&_c->io_lock);
            n = SSL_read(_c->ssl, _c->in.data + _c->in.len, 65536);
            int se = n > 0 ? SSL_ERROR_NONE : SSL_get_error(_c->ssl, n);
            if (se != SSL_ERROR_NONE && se != SSL_ERROR_WANT_READ && se != SSL_ERROR_WANT_WRITE &&
                se != SSL_ERROR_ZERO_RETURN)
                ssl_error(_err, _errlen, "TLS read");
            mapepire_mutex_unlock(&_c->io_lock);
            if (n > 0) break;
            if (se == SSL_ERROR_ZERO_RETURN) { mapepire_snprintf(_err, _errlen, "The server closed the connection"); return MAPEPIRE_WS_ECLOSED; }
            if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) {
                int r = wait_fd(_c->fd, se == SSL_ERROR_WANT_WRITE, _c->recv_timeout_ms);
                if (r < 0) { sock_err(_err, _errlen, "poll"); return MAPEPIRE_WS_EIO; }
                if (r == 0) { mapepire_snprintf(_err, _errlen, "Timed out waiting for the server"); return MAPEPIRE_WS_ETIMEOUT; }
                continue;
            }
            return MAPEPIRE_WS_EIO;
        }
#endif
        n = (int)recv(_c->fd, _c->in.data + _c->in.len, 65536, 0);
        if (n == 0) { mapepire_snprintf(_err, _errlen, "The server closed the connection"); return MAPEPIRE_WS_ECLOSED; }
        if (n < 0) {
            int e = sock_errno();
#ifdef _WIN32
            if (e == WSAETIMEDOUT) { mapepire_snprintf(_err, _errlen, "Timed out waiting for the server"); return MAPEPIRE_WS_ETIMEOUT; }
#else
            if (e == EINTR) continue;
            if (e == EAGAIN || e == EWOULDBLOCK) { mapepire_snprintf(_err, _errlen, "Timed out waiting for the server"); return MAPEPIRE_WS_ETIMEOUT; }
#endif
            sock_err(_err, _errlen, "recv");
            return MAPEPIRE_WS_EIO;
        }
        break;
    }
    _c->in.len += (size_t)n;
    _c->in.data[_c->in.len] = '\0';
    return MAPEPIRE_WS_OK;
}

/* Ensure at least n unread bytes are buffered. */
static int conn_need(conn_t *_c, size_t _n, char *_err, size_t _errlen)
{
    while (_c->in.len - _c->in_pos < _n) {
        int rc = conn_fill(_c, _err, _errlen);
        if (rc) return rc;
    }
    return MAPEPIRE_WS_OK;
}

/* Read the HTTP response head (through CRLFCRLF); returns malloc'd header text. */
static int read_http_head(conn_t *_c, char **_head, char *_err, size_t _errlen)
{
    for (;;) {
        const char *start = _c->in.data ? _c->in.data + _c->in_pos : NULL;
        size_t avail = _c->in.len - _c->in_pos;
        if (start) {
            for (size_t i = 0; i + 3 < avail; i++) {
                if (start[i] == '\r' && start[i + 1] == '\n' && start[i + 2] == '\r' && start[i + 3] == '\n') {
                    *_head = mapepire_strndup(start, i + 2);
                    _c->in_pos += i + 4;
                    return MAPEPIRE_WS_OK;
                }
            }
        }
        if (avail > 65536) { mapepire_snprintf(_err, _errlen, "HTTP response header too large"); return MAPEPIRE_WS_EPROTO; }
        int rc = conn_fill(_c, _err, _errlen);
        if (rc) return rc;
    }
}

static int http_status(const char *_head)
{
    const char *sp = strchr(_head, ' ');
    return sp ? atoi(sp + 1) : 0;
}

/* Case-insensitive header lookup; returns malloc'd trimmed value or NULL. */
static char *http_header(const char *_head, const char *_name)
{
    size_t nl = strlen(_name);
    const char *line = strstr(_head, "\r\n");
    while (line) {
        line += 2;
        if (!mapepire_strnicmp(line, _name, nl) && line[nl] == ':') {
            const char *v = line + nl + 1;
            const char *e = strstr(v, "\r\n");
            if (!e) e = v + strlen(v);
            char *val = mapepire_strndup(v, (size_t)(e - v));
            return mapepire_trim(val);
        }
        line = strstr(line, "\r\n");
    }
    return NULL;
}

static void append_host_header(mapepire_buf *_b, const sock_ws *_w)
{
    int ipv6 = strchr(_w->host, ':') != NULL;
    int dflt = (_w->p.secure && _w->p.port == 443) || (!_w->p.secure && _w->p.port == 80);
    mapepire_buf_printf(_b, "Host: %s%s%s", ipv6 ? "[" : "", _w->host, ipv6 ? "]" : "");
    if (!dflt) mapepire_buf_printf(_b, ":%d", _w->p.port);
    mapepire_buf_append_str(_b, "\r\n");
}

/* ---- frames ---------------------------------------------------------------- */

static int send_frame(conn_t *_c, int _opcode, const void *_data, size_t _len, char *_err, size_t _errlen)
{
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | _opcode);
    if (_len < 126) hdr[h++] = (uint8_t)(0x80 | _len);
    else if (_len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (uint8_t)(_len >> 8);
        hdr[h++] = (uint8_t)_len;
    } else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (uint8_t)((uint64_t)_len >> (8 * i));
    }
    uint8_t mask[4];
    mapepire_random_bytes(mask, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;
    mapepire_buf f;
    mapepire_buf_init(&f);
    if (mapepire_buf_reserve(&f, h + _len)) { mapepire_snprintf(_err, _errlen, "Out of memory"); return MAPEPIRE_WS_EIO; }
    mapepire_buf_append(&f, hdr, h);
    const uint8_t *src = (const uint8_t *)_data;
    uint8_t *dst = (uint8_t *)f.data + h;
    for (size_t i = 0; i < _len; i++) dst[i] = src[i] ^ mask[i & 3];
    f.len = h + _len;
    int rc = conn_write(_c, f.data, f.len, _err, _errlen);
    mapepire_buf_free(&f);
    return rc;
}

static int s_send(mapepire_ws *_ws, const char *_data, size_t _len, char *_err, size_t _errlen)
{
    sock_ws *w = (sock_ws *)_ws;
    mapepire_mutex_lock(&w->send_lock);
    int rc = send_frame(&w->c, 0x1, _data, _len, _err, _errlen);
    mapepire_mutex_unlock(&w->send_lock);
    return rc;
}

static int s_recv(mapepire_ws *_ws, mapepire_buf *_out, char *_err, size_t _errlen)
{
    sock_ws *w = (sock_ws *)_ws;
    conn_t *c = &w->c;
    int in_message = 0;
    for (;;) {
        int rc = conn_need(c, 2, _err, _errlen);
        if (rc) return rc;
        const uint8_t *b = (const uint8_t *)c->in.data + c->in_pos;
        int fin = b[0] & 0x80, opcode = b[0] & 0x0F, masked = b[1] & 0x80;
        uint64_t plen = b[1] & 0x7F;
        size_t hlen = 2;
        if (plen == 126) hlen += 2;
        else if (plen == 127) hlen += 8;
        if (masked) hlen += 4;
        if ((rc = conn_need(c, hlen, _err, _errlen))) return rc;
        b = (const uint8_t *)c->in.data + c->in_pos;
        if (plen == 126) plen = ((uint64_t)b[2] << 8) | b[3];
        else if (plen == 127) {
            plen = 0;
            for (int i = 0; i < 8; i++) plen = (plen << 8) | b[2 + i];
        }
        if (plen > ((uint64_t)1 << 40)) { mapepire_snprintf(_err, _errlen, "WebSocket frame too large"); return MAPEPIRE_WS_EPROTO; }
        if ((rc = conn_need(c, hlen + (size_t)plen, _err, _errlen))) return rc;
        b = (const uint8_t *)c->in.data + c->in_pos;
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) memcpy(mask, b + hlen - 4, 4);
        const uint8_t *payload = b + hlen;
        size_t n = (size_t)plen;

        if (opcode == 0x8) { /* close */
            unsigned code = n >= 2 ? (unsigned)((payload[0] ^ mask[0]) << 8 | (payload[1] ^ mask[1])) : 1005;
            c->in_pos += hlen + n;
            mapepire_mutex_lock(&w->send_lock);
            send_frame(c, 0x8, NULL, 0, _err, 0);
            mapepire_mutex_unlock(&w->send_lock);
            mapepire_snprintf(_err, _errlen, "The server closed the connection (code %u)", code);
            return MAPEPIRE_WS_ECLOSED;
        }
        if (opcode == 0x9) { /* ping -> pong with same payload */
            uint8_t *tmp = (uint8_t *)malloc(n ? n : 1);
            if (!tmp) return MAPEPIRE_WS_EIO;
            for (size_t i = 0; i < n; i++) tmp[i] = payload[i] ^ mask[i & 3];
            c->in_pos += hlen + n;
            mapepire_mutex_lock(&w->send_lock);
            rc = send_frame(c, 0xA, tmp, n, _err, _errlen);
            mapepire_mutex_unlock(&w->send_lock);
            free(tmp);
            if (rc) return rc;
            continue;
        }
        if (opcode == 0xA) { c->in_pos += hlen + n; continue; }
        if (opcode == 0x1 || opcode == 0x2) {
            if (in_message) { mapepire_snprintf(_err, _errlen, "Unexpected WebSocket data frame"); return MAPEPIRE_WS_EPROTO; }
            in_message = 1;
        } else if (opcode == 0x0) {
            if (!in_message) { mapepire_snprintf(_err, _errlen, "Unexpected WebSocket continuation frame"); return MAPEPIRE_WS_EPROTO; }
        } else {
            mapepire_snprintf(_err, _errlen, "Unknown WebSocket opcode %d", opcode);
            return MAPEPIRE_WS_EPROTO;
        }
        if (mapepire_buf_reserve(_out, n)) { mapepire_snprintf(_err, _errlen, "Out of memory"); return MAPEPIRE_WS_EIO; }
        uint8_t *dst = (uint8_t *)_out->data + _out->len;
        if (masked) for (size_t i = 0; i < n; i++) dst[i] = payload[i] ^ mask[i & 3];
        else memcpy(dst, payload, n);
        _out->len += n;
        _out->data[_out->len] = '\0';
        c->in_pos += hlen + n;
        if (fin) return MAPEPIRE_WS_OK;
    }
}

/* ---- HTTP GET (blob download) ------------------------------------------- */

static int s_http_get(mapepire_ws *_ws, const char *_path, mapepire_buf *_out, char *_err, size_t _errlen)
{
    sock_ws *w = (sock_ws *)_ws;
    conn_t c;
    int rc = conn_open(&c, &w->p, w->pin, 1, _err, _errlen);
    if (rc) return rc;
    mapepire_buf req;
    mapepire_buf_init(&req);
    mapepire_buf_printf(&req, "GET %s HTTP/1.1\r\n", _path);
    append_host_header(&req, w);
    mapepire_buf_printf(&req, "Authorization: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n\r\n", w->auth);
    rc = conn_write(&c, req.data, req.len, _err, _errlen);
    memset(req.data, 0, req.len);
    mapepire_buf_free(&req);
    char *head = NULL;
    if (!rc) rc = read_http_head(&c, &head, _err, _errlen);
    if (!rc && http_status(head) != 200) {
        mapepire_snprintf(_err, _errlen, "HTTP GET %s returned status %d", _path, http_status(head));
        rc = MAPEPIRE_WS_EPROTO;
    }
    if (!rc) {
        char *te = http_header(head, "Transfer-Encoding");
        char *cl = http_header(head, "Content-Length");
        if (te && strstr(te, "chunked")) {
            for (;;) {
                /* read chunk size line */
                const char *line;
                size_t avail;
                for (;;) {
                    line = c.in.data ? c.in.data + c.in_pos : "";
                    avail = c.in.len - c.in_pos;
                    const char *nl = avail ? (const char *)memchr(line, '\n', avail) : NULL;
                    if (nl) break;
                    if ((rc = conn_fill(&c, _err, _errlen))) break;
                }
                if (rc) break;
                size_t sz = (size_t)strtoul(line, NULL, 16);
                const char *nl = (const char *)memchr(line, '\n', avail);
                c.in_pos += (size_t)(nl - line) + 1;
                if (sz == 0) break;
                if ((rc = conn_need(&c, sz + 2, _err, _errlen))) break;
                mapepire_buf_append(_out, c.in.data + c.in_pos, sz);
                c.in_pos += sz + 2;
            }
        } else if (cl) {
            size_t want = (size_t)strtoull(cl, NULL, 10);
            rc = conn_need(&c, want, _err, _errlen);
            if (!rc) mapepire_buf_append(_out, c.in.data + c.in_pos, want);
        } else {
            for (;;) {
                mapepire_buf_append(_out, c.in.data + c.in_pos, c.in.len - c.in_pos);
                c.in_pos = c.in.len;
                int r = conn_fill(&c, _err, _errlen);
                if (r == MAPEPIRE_WS_ECLOSED) break;
                if (r) { rc = r; break; }
            }
        }
        free(te);
        free(cl);
    }
    free(head);
    conn_close(&c);
    return rc;
}

static void s_interrupt(mapepire_ws *_ws)
{
    sock_ws *w = (sock_ws *)_ws;
    /* shutdown() (unlike close()) is safe while another thread is blocked in recv/poll */
    if (w->c.fd != BAD_SOCK) {
#ifdef _WIN32
        shutdown(w->c.fd, SD_BOTH);
#else
        shutdown(w->c.fd, SHUT_RDWR);
#endif
    }
}

static void s_close(mapepire_ws *_ws)
{
    sock_ws *w = (sock_ws *)_ws;
    if (w->c.fd != BAD_SOCK) {
        uint8_t code[2] = {0x03, 0xE8}; /* 1000 normal closure */
        send_frame(&w->c, 0x8, code, 2, NULL, 0);
    }
    conn_close(&w->c);
    free(w->host);
    free(w->path);
    free(w->ca_file);
    if (w->auth) { memset(w->auth, 0, strlen(w->auth)); free(w->auth); }
    mapepire_mutex_destroy(&w->send_lock);
    free(w);
}

static const mapepire_ws_vtbl socket_vtbl = {s_send, s_recv, s_http_get, s_interrupt, s_close};

int mapepire_ws_open_socket(mapepire_ws **_out, const mapepire_ws_params *_p, char *_err, size_t _errlen)
{
    *_out = NULL;
    sock_ws *w = (sock_ws *)calloc(1, sizeof *w);
    if (!w) return MAPEPIRE_WS_EIO;
    w->base.vt = &socket_vtbl;
    w->c.fd = BAD_SOCK;
    mapepire_mutex_init(&w->send_lock);
    w->p = *_p;
    w->host = mapepire_strdup(_p->host);
    w->path = mapepire_strdup(_p->path && *_p->path ? _p->path : "/");
    w->ca_file = mapepire_strdup(_p->ca_file);
    w->p.host = w->host;
    w->p.path = w->path;
    w->p.ca_file = w->ca_file;
    w->p.user = w->p.password = w->p.cert_sha256 = NULL;
    w->auth = mapepire_ws_basic_auth(_p->user, _p->password);
    int rc;
    if (_p->cert_sha256 && *_p->cert_sha256 && mapepire_ws_normalize_fingerprint(_p->cert_sha256, w->pin)) {
        mapepire_snprintf(_err, _errlen, "CertSHA256 must be 64 hexadecimal digits (colons allowed)");
        rc = MAPEPIRE_WS_ETLS;
        goto fail;
    }

    rc = conn_open(&w->c, &w->p, w->pin, 0, _err, _errlen);
    if (rc) goto fail;

    uint8_t nonce[16];
    mapepire_random_bytes(nonce, sizeof nonce);
    char *key = mapepire_base64_encode(nonce, sizeof nonce);
    mapepire_buf req;
    mapepire_buf_init(&req);
    mapepire_buf_printf(&req, "GET %s HTTP/1.1\r\n", w->path);
    append_host_header(&req, w);
    mapepire_buf_printf(&req,
                  "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                  "Sec-WebSocket-Version: 13\r\nAuthorization: %s\r\nUser-Agent: mapepire-odbc\r\n\r\n",
                  key, w->auth);
    rc = conn_write(&w->c, req.data, req.len, _err, _errlen);
    memset(req.data, 0, req.len);
    mapepire_buf_free(&req);
    if (rc) { free(key); goto fail; }

    char *head = NULL;
    rc = read_http_head(&w->c, &head, _err, _errlen);
    if (rc) { free(key); goto fail; }
    int status = http_status(head);
    if (status != 101) {
        if (status == 401 || status == 403 || status == 404) {
            mapepire_snprintf(_err, _errlen,
                        "The server refused the WebSocket connection (HTTP %d). Check the user ID, password "
                        "and that the server is a Mapepire server",
                        status);
            rc = MAPEPIRE_WS_EAUTH;
        } else {
            mapepire_snprintf(_err, _errlen, "The server refused the WebSocket upgrade (HTTP %d)", status);
            rc = MAPEPIRE_WS_EPROTO;
        }
        free(head);
        free(key);
        goto fail;
    }
    /* Validate Sec-WebSocket-Accept */
    mapepire_buf k;
    mapepire_buf_init(&k);
    mapepire_buf_append_str(&k, key);
    mapepire_buf_append_str(&k, WS_GUID);
    uint8_t digest[20];
    mapepire_sha1(k.data, k.len, digest);
    mapepire_buf_free(&k);
    char *expect = mapepire_base64_encode(digest, sizeof digest);
    char *accept = http_header(head, "Sec-WebSocket-Accept");
    int ok = accept && expect && !strcmp(accept, expect);
    free(accept);
    free(expect);
    free(key);
    free(head);
    if (!ok) {
        mapepire_snprintf(_err, _errlen, "Invalid Sec-WebSocket-Accept in the server handshake");
        rc = MAPEPIRE_WS_EPROTO;
        goto fail;
    }
    *_out = &w->base;
    return MAPEPIRE_WS_OK;

fail:
    conn_close(&w->c);
    w->c.fd = BAD_SOCK;
    s_close(&w->base);
    return rc;
}

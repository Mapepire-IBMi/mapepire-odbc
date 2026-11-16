#include "mapepire_ws.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int mapepire_ws_open(mapepire_ws **_out, const mapepire_ws_params *_p, char *_err, size_t _errlen)
{
    *_out = NULL;
    mapepire_ws_backend be = _p->backend;
#ifdef _WIN32
    if (be == MAPEPIRE_WS_BACKEND_DEFAULT) be = MAPEPIRE_WS_BACKEND_WINHTTP;
    if (be == MAPEPIRE_WS_BACKEND_WINHTTP) return mapepire_ws_open_winhttp(_out, _p, _err, _errlen);
#else
    if (be == MAPEPIRE_WS_BACKEND_WINHTTP) {
        mapepire_snprintf(_err, _errlen, "The WinHTTP transport is only available on Windows");
        return MAPEPIRE_WS_EUNSUPPORTED;
    }
#endif
    return mapepire_ws_open_socket(_out, _p, _err, _errlen);
}

int mapepire_ws_send(mapepire_ws *_ws, const char *_data, size_t _len, char *_err, size_t _errlen)
{
    return _ws->vt->send(_ws, _data, _len, _err, _errlen);
}

int mapepire_ws_recv(mapepire_ws *_ws, mapepire_buf *_out, char *_err, size_t _errlen)
{
    mapepire_buf_clear(_out);
    return _ws->vt->recv(_ws, _out, _err, _errlen);
}

int mapepire_ws_http_get(mapepire_ws *_ws, const char *_path, mapepire_buf *_out, char *_err, size_t _errlen)
{
    mapepire_buf_clear(_out);
    return _ws->vt->http_get(_ws, _path, _out, _err, _errlen);
}

void mapepire_ws_interrupt(mapepire_ws *_ws)
{
    if (_ws) _ws->vt->interrupt(_ws);
}

void mapepire_ws_close(mapepire_ws *_ws)
{
    if (_ws) _ws->vt->close(_ws);
}

char *mapepire_ws_basic_auth(const char *_user, const char *_password)
{
    mapepire_buf b;
    mapepire_buf_init(&b);
    mapepire_buf_append_str(&b, _user ? _user : "");
    mapepire_buf_append_char(&b, ':');
    mapepire_buf_append_str(&b, _password ? _password : "");
    char *enc = mapepire_base64_encode(b.data, b.len);
    /* scrub the plaintext credentials */
    if (b.data) memset(b.data, 0, b.len);
    mapepire_buf_free(&b);
    if (!enc) return NULL;
    mapepire_buf_append_str(&b, "Basic ");
    mapepire_buf_append_str(&b, enc);
    memset(enc, 0, strlen(enc));
    free(enc);
    return mapepire_buf_detach(&b);
}

int mapepire_ws_normalize_fingerprint(const char *_in, char _out[65])
{
    size_t n = 0;
    for (const char *p = _in; *p; p++) {
        if (*p == ':' || *p == ' ' || *p == '-') continue;
        if (!isxdigit((unsigned char)*p) || n >= 64) return -1;
        _out[n++] = (char)tolower((unsigned char)*p);
    }
    _out[n] = '\0';
    return n == 64 ? 0 : -1;
}

void mapepire_ws_hex32(const unsigned char *_digest, char _out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        _out[i * 2] = hex[_digest[i] >> 4];
        _out[i * 2 + 1] = hex[_digest[i] & 15];
    }
    _out[64] = '\0';
}

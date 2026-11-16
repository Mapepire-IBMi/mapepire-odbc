/*
 * Connection string / DSN keyword handling.
 */
#include "drv.h"

#ifdef _WIN32
#include <odbcinst.h>
#else
#include <odbcinst.h>
#endif

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *canon;
    const char *aliases[5];
} keyword;

/* Canonical keywords (upper case) and accepted aliases. */
static const keyword KEYWORDS[] = {
    {"DSN", {NULL}},
    {"DRIVER", {NULL}},
    {"SERVER", {"HOST", "SYSTEM", "HOSTNAME", NULL}},
    {"PORT", {NULL}},
    {"UID", {"USER", "USERID", "USERNAME", NULL}},
    {"PWD", {"PASSWORD", NULL}},
    {"SECURE", {"SSL", "TLS", NULL}},
    {"TRUSTSERVERCERTIFICATE", {"TRUSTCERT", "ALLOWSELFSIGNED", NULL}},
    {"CERTSHA256", {"FINGERPRINT", "CERTFINGERPRINT", NULL}},
    {"CAFILE", {"CA", NULL}},
    {"PROPS", {"JDBCOPTIONS", "PROPERTIES", NULL}},
    {"TECHNIQUE", {NULL}},
    {"APPLICATION", {"APP", NULL}},
    {"NAMING", {NULL}},
    {"LIBRARIES", {"DEFAULTLIBRARIES", "DBQ", "LIBRARYLIST", NULL}},
    {"BLOCKSIZE", {"FETCHSIZE", "ROWS", NULL}},
    {"TRANSPORT", {NULL}},
    {"CONNECTTIMEOUT", {"LOGINTIMEOUT", NULL}},
    {"SOCKETTIMEOUT", {"TIMEOUT", NULL}},
    {"CHARENCODING", {NULL}},
    {"READONLY", {NULL}},
    {"AUTOCOMMIT", {NULL}},
    {"PATH", {"WSPATH", NULL}},
    {NULL, {NULL}}};

const char *mapepire_conf_canonical(const char *_key)
{
    for (const keyword *k = KEYWORDS; k->canon; k++) {
        if (!mapepire_stricmp(_key, k->canon)) return k->canon;
        for (int i = 0; k->aliases[i]; i++)
            if (!mapepire_stricmp(_key, k->aliases[i])) return k->canon;
    }
    return NULL;
}

void mapepire_conf_free(mapepire_conf *_c)
{
    for (int i = 0; i < _c->count; i++) {
        free(_c->items[i].key);
        if (_c->items[i].value) {
            memset(_c->items[i].value, 0, strlen(_c->items[i].value));
            free(_c->items[i].value);
        }
    }
    free(_c->items);
    _c->items = NULL;
    _c->count = _c->cap = 0;
}

const char *mapepire_conf_get(const mapepire_conf *_c, const char *_key)
{
    const char *k = mapepire_conf_canonical(_key);
    if (!k) k = _key;
    for (int i = 0; i < _c->count; i++)
        if (!mapepire_stricmp(_c->items[i].key, k)) return _c->items[i].value;
    return NULL;
}

void mapepire_conf_set(mapepire_conf *_c, const char *_key, const char *_value)
{
    const char *canon = mapepire_conf_canonical(_key);
    char *k;
    if (canon) {
        k = mapepire_strdup(canon);
    } else {
        /* keep unknown keywords (upper-cased) so they round-trip */
        k = mapepire_strdup(_key);
        for (char *p = k; p && *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    }
    if (!k) return;
    for (int i = 0; i < _c->count; i++) {
        if (!strcmp(_c->items[i].key, k)) {
            free(k);
            free(_c->items[i].value);
            _c->items[i].value = mapepire_strdup(_value ? _value : "");
            return;
        }
    }
    if (_c->count == _c->cap) {
        int ncap = _c->cap ? _c->cap * 2 : 16;
        mapepire_kv *ni = (mapepire_kv *)realloc(_c->items, (size_t)ncap * sizeof *ni);
        if (!ni) { free(k); return; }
        _c->items = ni;
        _c->cap = ncap;
    }
    _c->items[_c->count].key = k;
    _c->items[_c->count].value = mapepire_strdup(_value ? _value : "");
    _c->count++;
}

int mapepire_conf_parse(mapepire_conf *_c, const char *_s, char *_err, size_t _errlen)
{
    if (!_s) return 0;
    const char *p = _s;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '\t') p++;
        if (!*p) break;
        const char *ks = p;
        while (*p && *p != '=' && *p != ';') p++;
        if (*p != '=') {
            mapepire_snprintf(_err, _errlen, "Invalid connection string attribute near '%.*s'", (int)(p - ks), ks);
            return -1;
        }
        char *key = mapepire_strndup(ks, (size_t)(p - ks));
        mapepire_trim(key);
        p++; /* '=' */
        while (*p == ' ' || *p == '\t') p++;
        mapepire_buf val;
        mapepire_buf_init(&val);
        if (*p == '{') {
            p++;
            for (;;) {
                if (!*p) {
                    mapepire_snprintf(_err, _errlen, "Unterminated '{' in the value of '%s'", key);
                    free(key);
                    mapepire_buf_free(&val);
                    return -1;
                }
                if (*p == '}') {
                    if (p[1] == '}') { mapepire_buf_append_char(&val, '}'); p += 2; continue; }
                    p++;
                    break;
                }
                mapepire_buf_append_char(&val, *p++);
            }
            while (*p && *p != ';') p++;
        } else {
            const char *vs = p;
            while (*p && *p != ';') p++;
            mapepire_buf_append(&val, vs, (size_t)(p - vs));
        }
        char *v = mapepire_buf_detach(&val);
        if (key[0] != '{') mapepire_trim(v);
        if (key[0]) mapepire_conf_set(_c, key, v);
        memset(v, 0, strlen(v));
        free(v);
        free(key);
    }
    return 0;
}

void mapepire_conf_load_dsn(mapepire_conf *_c, const char *_dsn)
{
    if (!_dsn || !*_dsn) return;
#ifdef _WIN32
    size_t wl = mapepire_utf8_to_utf16(_dsn, strlen(_dsn), NULL, 0);
    WCHAR *wdsn = (WCHAR *)calloc(wl + 1, sizeof(WCHAR));
    if (!wdsn) return;
    mapepire_utf8_to_utf16(_dsn, strlen(_dsn), (uint16_t *)wdsn, wl + 1);
    WCHAR keys[8192];
    int n = SQLGetPrivateProfileStringW(wdsn, NULL, L"", keys, (int)(sizeof keys / sizeof keys[0]), L"ODBC.INI");
    for (int i = 0; i < n && keys[i];) {
        WCHAR *wk = &keys[i];
        size_t klen = wcslen(wk);
        WCHAR wv[4096];
        SQLGetPrivateProfileStringW(wdsn, wk, L"", wv, (int)(sizeof wv / sizeof wv[0]), L"ODBC.INI");
        char *k = mapepire_utf16_to_utf8((const uint16_t *)wk, -1, NULL);
        char *v = mapepire_utf16_to_utf8((const uint16_t *)wv, -1, NULL);
        if (k && v && !mapepire_conf_get(_c, k) && mapepire_stricmp(k, "Driver") != 0) mapepire_conf_set(_c, k, v);
        if (v) { memset(v, 0, strlen(v)); free(v); }
        free(k);
        SecureZeroMemory(wv, sizeof wv);
        i += (int)klen + 1;
    }
    free(wdsn);
#else
    char keys[8192];
    int n = SQLGetPrivateProfileString(_dsn, NULL, "", keys, (int)sizeof keys, "ODBC.INI");
    for (int i = 0; i < n && keys[i];) {
        const char *k = &keys[i];
        size_t klen = strlen(k);
        char v[4096];
        SQLGetPrivateProfileString(_dsn, k, "", v, (int)sizeof v, "ODBC.INI");
        if (!mapepire_conf_get(_c, k) && mapepire_stricmp(k, "Driver") != 0) mapepire_conf_set(_c, k, v);
        memset(v, 0, sizeof v);
        i += (int)klen + 1;
    }
#endif
}

char *mapepire_conf_to_string(const mapepire_conf *_c)
{
    mapepire_buf b;
    mapepire_buf_init(&b);
    for (int i = 0; i < _c->count; i++) {
        const char *v = _c->items[i].value ? _c->items[i].value : "";
        /* DSN-less connections must keep DRIVER; with a DSN the DM adds it */
        mapepire_buf_append_str(&b, _c->items[i].key);
        mapepire_buf_append_char(&b, '=');
        if (strpbrk(v, ";{}= ") || !*v) {
            mapepire_buf_append_char(&b, '{');
            for (const char *p = v; *p; p++) {
                if (*p == '}') mapepire_buf_append_char(&b, '}');
                mapepire_buf_append_char(&b, *p);
            }
            mapepire_buf_append_char(&b, '}');
        } else {
            mapepire_buf_append_str(&b, v);
        }
        mapepire_buf_append_char(&b, ';');
    }
    return mapepire_buf_detach(&b);
}

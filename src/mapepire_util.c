#include "mapepire_util.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#ifdef _WIN32
#include <bcrypt.h>
#include <process.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#endif

/* ---- buffer ------------------------------------------------------------- */

void mapepire_buf_init(mapepire_buf *_b) { _b->data = NULL; _b->len = 0; _b->cap = 0; }

void mapepire_buf_free(mapepire_buf *_b) { free(_b->data); mapepire_buf_init(_b); }

void mapepire_buf_clear(mapepire_buf *_b) { _b->len = 0; if (_b->data) _b->data[0] = '\0'; }

int mapepire_buf_reserve(mapepire_buf *_b, size_t _extra)
{
    size_t need = _b->len + _extra + 1;
    if (need <= _b->cap) return 0;
    size_t ncap = _b->cap ? _b->cap : 64;
    while (ncap < need) {
        if (ncap > ((size_t)-1) / 2) return -1;
        ncap *= 2;
    }
    char *nd = (char *)realloc(_b->data, ncap);
    if (!nd) return -1;
    _b->data = nd;
    _b->cap = ncap;
    return 0;
}

int mapepire_buf_append(mapepire_buf *_b, const void *_data, size_t _len)
{
    if (mapepire_buf_reserve(_b, _len)) return -1;
    if (_len) memcpy(_b->data + _b->len, _data, _len);
    _b->len += _len;
    _b->data[_b->len] = '\0';
    return 0;
}

int mapepire_buf_append_str(mapepire_buf *_b, const char *_s) { return mapepire_buf_append(_b, _s, strlen(_s)); }

int mapepire_buf_append_char(mapepire_buf *_b, char _c) { return mapepire_buf_append(_b, &_c, 1); }

int mapepire_buf_printf(mapepire_buf *_b, const char *_fmt, ...)
{
    va_list ap;
    va_start(ap, _fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, _fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return -1; }
    if (mapepire_buf_reserve(_b, (size_t)n)) { va_end(ap2); return -1; }
    vsnprintf(_b->data + _b->len, (size_t)n + 1, _fmt, ap2);
    va_end(ap2);
    _b->len += (size_t)n;
    return 0;
}

char *mapepire_buf_detach(mapepire_buf *_b)
{
    char *d = _b->data;
    if (!d) d = mapepire_strdup("");
    mapepire_buf_init(_b);
    return d;
}

/* ---- strings ------------------------------------------------------------ */

char *mapepire_strdup(const char *_s) { return _s ? mapepire_strndup(_s, strlen(_s)) : NULL; }

char *mapepire_strndup(const char *_s, size_t _n)
{
    char *d = (char *)malloc(_n + 1);
    if (!d) return NULL;
    if (_n) memcpy(d, _s, _n);
    d[_n] = '\0';
    return d;
}

int mapepire_stricmp(const char *_a, const char *_b)
{
    for (;; _a++, _b++) {
        int ca = tolower((unsigned char)*_a), cb = tolower((unsigned char)*_b);
        if (ca != cb || !ca) return ca - cb;
    }
}

int mapepire_strnicmp(const char *_a, const char *_b, size_t _n)
{
    for (; _n; _n--, _a++, _b++) {
        int ca = tolower((unsigned char)*_a), cb = tolower((unsigned char)*_b);
        if (ca != cb || !ca) return ca - cb;
    }
    return 0;
}

char *mapepire_trim(char *_s)
{
    if (!_s) return _s;
    char *p = _s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != _s) memmove(_s, p, strlen(p) + 1);
    size_t n = strlen(_s);
    while (n && isspace((unsigned char)_s[n - 1])) _s[--n] = '\0';
    return _s;
}

int mapepire_parse_bool(const char *_s, int _dflt)
{
    if (!_s || !*_s) return _dflt;
    if (!mapepire_stricmp(_s, "1") || !mapepire_stricmp(_s, "yes") || !mapepire_stricmp(_s, "true") ||
        !mapepire_stricmp(_s, "on") || !mapepire_stricmp(_s, "y"))
        return 1;
    if (!mapepire_stricmp(_s, "0") || !mapepire_stricmp(_s, "no") || !mapepire_stricmp(_s, "false") ||
        !mapepire_stricmp(_s, "off") || !mapepire_stricmp(_s, "n"))
        return 0;
    return _dflt;
}

void mapepire_snprintf(char *_dst, size_t _cap, const char *_fmt, ...)
{
    if (!_cap) return;
    va_list ap;
    va_start(ap, _fmt);
    vsnprintf(_dst, _cap, _fmt, ap);
    va_end(ap);
    _dst[_cap - 1] = '\0';
}

/* ---- Unicode ------------------------------------------------------------ */

char *mapepire_utf16_to_utf8(const uint16_t *_s, long _len, size_t *_out_len)
{
    size_t n = 0;
    if (!_s) _len = 0;
    else if (_len < 0) { _len = 0; while (_s[_len]) _len++; }
    /* worst case 3 bytes per unit (surrogate pairs: 4 bytes per 2 units) */
    if ((size_t)_len > (((size_t)-1) - 1) / 3) return NULL;
    char *out = (char *)malloc((size_t)_len * 3 + 1);
    if (!out) return NULL;
    for (long i = 0; i < _len; i++) {
        uint32_t c = _s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < _len && _s[i + 1] >= 0xDC00 && _s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (_s[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) {
            out[n++] = (char)(0xC0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[n++] = (char)(0xE0 | (c >> 12));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[n++] = (char)(0xF0 | (c >> 18));
            out[n++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[n] = '\0';
    if (_out_len) *_out_len = n;
    return out;
}

/* Decode one UTF-8 sequence at s[*i]; advances *i.  Invalid -> U+FFFD. */
static uint32_t utf8_next(const unsigned char *_s, size_t _len, size_t *_i)
{
    unsigned char c = _s[*_i];
    uint32_t cp;
    int extra;
    if (c < 0x80) { (*_i)++; return c; }
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { (*_i)++; return 0xFFFD; }
    if (*_i + (size_t)extra >= _len) { (*_i)++; return 0xFFFD; }
    for (int k = 1; k <= extra; k++) {
        unsigned char cc = _s[*_i + (size_t)k];
        if ((cc & 0xC0) != 0x80) { (*_i)++; return 0xFFFD; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *_i += (size_t)extra + 1;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

size_t mapepire_utf8_to_utf16(const char *_s, size_t _len, uint16_t *_dst, size_t _cap)
{
    const unsigned char *u = (const unsigned char *)_s;
    size_t i = 0, n = 0, written = 0;
    int full = (_dst == NULL || _cap == 0);
    while (i < _len) {
        uint32_t cp = utf8_next(u, _len, &i);
        size_t w = cp >= 0x10000 ? 2 : 1;
        if (!full && written + w <= _cap - 1) {
            if (w == 2) {
                cp -= 0x10000;
                _dst[written] = (uint16_t)(0xD800 + (cp >> 10));
                _dst[written + 1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            } else {
                _dst[written] = (uint16_t)cp;
            }
            written += w;
        } else {
            full = 1; /* never split a surrogate pair or skip ahead */
        }
        n += w;
    }
    if (_dst && _cap) _dst[written] = 0;
    return n;
}

char *mapepire_utf8_sanitize(const char *_s, size_t _len, size_t *_out_len)
{
    const unsigned char *u = (const unsigned char *)_s;
    mapepire_buf b;
    mapepire_buf_init(&b);
    if (mapepire_buf_reserve(&b, _len)) return NULL;
    size_t i = 0;
    while (i < _len) {
        size_t start = i;
        uint32_t cp = utf8_next(u, _len, &i);
        if (cp == 0xFFFD && !(i - start == 3 && u[start] == 0xEF && u[start + 1] == 0xBF && u[start + 2] == 0xBD))
            mapepire_buf_append(&b, "\xEF\xBF\xBD", 3);
        else
            mapepire_buf_append(&b, _s + start, i - start);
    }
    if (_out_len) *_out_len = b.len;
    return mapepire_buf_detach(&b);
}

size_t mapepire_utf8_offset_for_utf16_units(const char *_s, size_t _len, size_t _units)
{
    const unsigned char *u = (const unsigned char *)_s;
    size_t i = 0, n = 0;
    while (i < _len) {
        size_t save = i;
        uint32_t cp = utf8_next(u, _len, &i);
        size_t w = cp >= 0x10000 ? 2 : 1;
        if (n + w > _units) return save;
        n += w;
    }
    return _len;
}

/* ---- SHA-1 (RFC 3174), only used for Sec-WebSocket-Accept ------------------ */

#define ROL(_v, _b) (((_v) << (_b)) | ((_v) >> (32 - (_b))))

static void sha1_block(uint32_t _h[5], const uint8_t *_p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)_p[i * 4] << 24) | ((uint32_t)_p[i * 4 + 1] << 16) |
               ((uint32_t)_p[i * 4 + 2] << 8) | _p[i * 4 + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = _h[0], b = _h[1], c = _h[2], d = _h[3], e = _h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL(b, 30); b = a; a = t;
    }
    _h[0] += a; _h[1] += b; _h[2] += c; _h[3] += d; _h[4] += e;
}

void mapepire_sha1(const void *_data, size_t _len, uint8_t _out[20])
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const uint8_t *p = (const uint8_t *)_data;
    size_t full = _len / 64 * 64;
    for (size_t i = 0; i < full; i += 64) sha1_block(h, p + i);
    uint8_t tail[128];
    size_t rem = _len - full;
    memcpy(tail, p + full, rem);
    tail[rem++] = 0x80;
    size_t tlen = rem <= 56 ? 64 : 128;
    memset(tail + rem, 0, tlen - rem);
    uint64_t bits = (uint64_t)_len * 8;
    for (int i = 0; i < 8; i++) tail[tlen - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t i = 0; i < tlen; i += 64) sha1_block(h, tail + i);
    for (int i = 0; i < 5; i++) {
        _out[i * 4] = (uint8_t)(h[i] >> 24);
        _out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        _out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        _out[i * 4 + 3] = (uint8_t)h[i];
    }
}

char *mapepire_base64_encode(const void *_data, size_t _len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = (const uint8_t *)_data;
    char *out = (char *)malloc((_len + 2) / 3 * 4 + 1);
    if (!out) return NULL;
    size_t o = 0, i = 0;
    for (; i + 2 < _len; i += 3) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = tbl[v >> 18]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = tbl[(v >> 6) & 63]; out[o++] = tbl[v & 63];
    }
    if (i < _len) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < _len) v |= (uint32_t)p[i + 1] << 8;
        out[o++] = tbl[v >> 18]; out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < _len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

void mapepire_random_bytes(void *_buf, size_t _len)
{
#ifdef _WIN32
    if (BCryptGenRandom(NULL, (PUCHAR)_buf, (ULONG)_len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0) return;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < _len) {
            ssize_t r = read(fd, (char *)_buf + got, _len - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
        if (got == _len) return;
    }
#endif
    /* Fallback: only used for WebSocket masking keys, which need not be secret. */
    static unsigned long seed = 0x12345678UL;
    for (size_t i = 0; i < _len; i++) {
        seed = seed * 1103515245UL + 12345UL;
        ((uint8_t *)_buf)[i] = (uint8_t)(seed >> 16);
    }
}

/* ---- mutex -------------------------------------------------------------- */

#ifdef _WIN32
void mapepire_mutex_init(mapepire_mutex *_m) { InitializeCriticalSection(_m); }
void mapepire_mutex_destroy(mapepire_mutex *_m) { DeleteCriticalSection(_m); }
void mapepire_mutex_lock(mapepire_mutex *_m) { EnterCriticalSection(_m); }
void mapepire_mutex_unlock(mapepire_mutex *_m) { LeaveCriticalSection(_m); }
#else
void mapepire_mutex_init(mapepire_mutex *_m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(_m, &a);
    pthread_mutexattr_destroy(&a);
}
void mapepire_mutex_destroy(mapepire_mutex *_m) { pthread_mutex_destroy(_m); }
void mapepire_mutex_lock(mapepire_mutex *_m) { pthread_mutex_lock(_m); }
void mapepire_mutex_unlock(mapepire_mutex *_m) { pthread_mutex_unlock(_m); }
#endif

/* ---- condition variable and threads -------------------------------------- */

typedef struct {
    void (*fn)(void *);
    void *arg;
} thread_start;

#ifdef _WIN32
void mapepire_cond_init(mapepire_cond *_c) { InitializeConditionVariable(_c); }
void mapepire_cond_destroy(mapepire_cond *_c) { (void)_c; }
int mapepire_cond_wait(mapepire_cond *_c, mapepire_mutex *_m, long _ms)
{
    if (SleepConditionVariableCS(_c, _m, _ms < 0 ? INFINITE : (DWORD)_ms)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? 1 : 0;
}
void mapepire_cond_broadcast(mapepire_cond *_c) { WakeAllConditionVariable(_c); }
long long mapepire_now_ms(void) { return (long long)GetTickCount64(); }

static unsigned __stdcall thread_tramp(void *_p)
{
    thread_start st = *(thread_start *)_p;
    free(_p);
    st.fn(st.arg);
    return 0;
}

int mapepire_thread_start(mapepire_thread *_t, void (*_fn)(void *), void *_arg)
{
    thread_start *st = (thread_start *)malloc(sizeof *st);
    if (!st) return -1;
    st->fn = _fn;
    st->arg = _arg;
    uintptr_t h = _beginthreadex(NULL, 0, thread_tramp, st, 0, NULL);
    if (!h) { free(st); return -1; }
    *_t = (HANDLE)h;
    return 0;
}

void mapepire_thread_join(mapepire_thread *_t)
{
    WaitForSingleObject(*_t, INFINITE);
    CloseHandle(*_t);
}
#else
void mapepire_cond_init(mapepire_cond *_c)
{
    pthread_condattr_t a;
    pthread_condattr_init(&a);
#if !defined(__APPLE__)
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
#endif
    pthread_cond_init(_c, &a);
    pthread_condattr_destroy(&a);
}
void mapepire_cond_destroy(mapepire_cond *_c) { pthread_cond_destroy(_c); }
int mapepire_cond_wait(mapepire_cond *_c, mapepire_mutex *_m, long _ms)
{
    if (_ms < 0) {
        pthread_cond_wait(_c, _m);
        return 0;
    }
    struct timespec ts;
#if defined(__APPLE__)
    clock_gettime(CLOCK_REALTIME, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    ts.tv_sec += _ms / 1000;
    ts.tv_nsec += (_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(_c, _m, &ts) == ETIMEDOUT ? 1 : 0;
}
void mapepire_cond_broadcast(mapepire_cond *_c) { pthread_cond_broadcast(_c); }
long long mapepire_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void *thread_tramp(void *_p)
{
    thread_start st = *(thread_start *)_p;
    free(_p);
    st.fn(st.arg);
    return NULL;
}

int mapepire_thread_start(mapepire_thread *_t, void (*_fn)(void *), void *_arg)
{
    thread_start *st = (thread_start *)malloc(sizeof *st);
    if (!st) return -1;
    st->fn = _fn;
    st->arg = _arg;
    if (pthread_create(_t, NULL, thread_tramp, st)) { free(st); return -1; }
    return 0;
}

void mapepire_thread_join(mapepire_thread *_t) { pthread_join(*_t, NULL); }
#endif

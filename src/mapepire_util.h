/*
 * Small, dependency-free helpers shared by the whole driver: growable byte
 * buffers, UTF-8 <-> UTF-16 conversion, case-insensitive comparison, SHA-1,
 * Base64 and a portable mutex.
 */
#ifndef MAPEPIRE_UTIL_H
#define MAPEPIRE_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef CRITICAL_SECTION   mapepire_mutex;
typedef CONDITION_VARIABLE mapepire_cond;
typedef HANDLE             mapepire_thread;
#else
#include <pthread.h>
typedef pthread_mutex_t mapepire_mutex;
typedef pthread_cond_t  mapepire_cond;
typedef pthread_t       mapepire_thread;
#endif

/* ---- growable buffer --------------------------------------------------- */

typedef struct mapepire_buf {
    char  *data;   /* always NUL terminated when non-NULL */
    size_t len;
    size_t cap;
} mapepire_buf;

void mapepire_buf_init(mapepire_buf *_b);
void mapepire_buf_free(mapepire_buf *_b);
void mapepire_buf_clear(mapepire_buf *_b);
int  mapepire_buf_reserve(mapepire_buf *_b, size_t _extra);           /* 0 on success */
int  mapepire_buf_append(mapepire_buf *_b, const void *_data, size_t _len);
int  mapepire_buf_append_str(mapepire_buf *_b, const char *_s);
int  mapepire_buf_append_char(mapepire_buf *_b, char _c);
int  mapepire_buf_printf(mapepire_buf *_b, const char *_fmt, ...);
/* Detach the data (caller frees with free()); leaves the buffer empty. */
char *mapepire_buf_detach(mapepire_buf *_b);

/* ---- strings ------------------------------------------------------------ */

char *mapepire_strdup(const char *_s);
char *mapepire_strndup(const char *_s, size_t _n);
int   mapepire_stricmp(const char *_a, const char *_b);
int   mapepire_strnicmp(const char *_a, const char *_b, size_t _n);
/* Trim ASCII whitespace in place; returns s. */
char *mapepire_trim(char *_s);
/* Parse a boolean-ish connection keyword value (yes/no/true/false/1/0/on/off). */
int   mapepire_parse_bool(const char *_s, int _dflt);
/* snprintf that always terminates and never fails on truncation. */
void  mapepire_snprintf(char *_dst, size_t _cap, const char *_fmt, ...);

/* ---- Unicode ------------------------------------------------------------ */

/*
 * Convert UTF-16 (len units, or -1 for NUL terminated) to a freshly
 * allocated, NUL terminated UTF-8 string.  *out_len receives the byte
 * length when non-NULL.  Invalid surrogates become U+FFFD.
 */
char *mapepire_utf16_to_utf8(const uint16_t *_s, long _len, size_t *_out_len);

/*
 * Convert UTF-8 (len bytes) to UTF-16.  Writes at most cap units into dst
 * (which may be NULL when cap is 0) and always NUL terminates when cap > 0.
 * Returns the total number of UTF-16 units the full conversion needs
 * (excluding the terminator), so callers can detect truncation.
 */
size_t mapepire_utf8_to_utf16(const char *_s, size_t _len, uint16_t *_dst, size_t _cap);

/* Copy s, replacing invalid UTF-8 sequences with U+FFFD (malloc'd, NUL terminated). */
char *mapepire_utf8_sanitize(const char *_s, size_t _len, size_t *_out_len);

/*
 * Count the code units needed to hold s as UTF-16, and the byte offset in
 * s at which `units` UTF-16 units have been consumed (for chunked SQLGetData).
 */
size_t mapepire_utf8_offset_for_utf16_units(const char *_s, size_t _len, size_t _units);

/* ---- crypto-ish helpers for the WebSocket handshake ---------------------- */

void   mapepire_sha1(const void *_data, size_t _len, uint8_t _out[20]);
/* Base64 encode; returns malloc'd NUL-terminated string. */
char  *mapepire_base64_encode(const void *_data, size_t _len);
/* Fill buf with random bytes (OS CSPRNG when available). */
void   mapepire_random_bytes(void *_buf, size_t _len);

/* ---- mutex -------------------------------------------------------------- */

void mapepire_mutex_init(mapepire_mutex *_m);
void mapepire_mutex_destroy(mapepire_mutex *_m);
void mapepire_mutex_lock(mapepire_mutex *_m);
void mapepire_mutex_unlock(mapepire_mutex *_m);

/* ---- condition variable and threads -------------------------------------- */

void mapepire_cond_init(mapepire_cond *_c);
void mapepire_cond_destroy(mapepire_cond *_c);
/* Wait on c with m held (locked exactly once).  ms < 0 waits forever.
 * Returns 0 when signalled (or spuriously woken), 1 on timeout. */
int  mapepire_cond_wait(mapepire_cond *_c, mapepire_mutex *_m, long _ms);
void mapepire_cond_broadcast(mapepire_cond *_c);
/* Monotonic milliseconds, for computing deadlines. */
long long mapepire_now_ms(void);

int  mapepire_thread_start(mapepire_thread *_t, void (*_fn)(void *), void *_arg);   /* 0 on success */
void mapepire_thread_join(mapepire_thread *_t);

#endif

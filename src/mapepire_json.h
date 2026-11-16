/*
 * Minimal JSON DOM.  Numbers keep their original token text so DECIMAL values
 * from Db2 are never rounded through a double.
 */
#ifndef MAPEPIRE_JSON_H
#define MAPEPIRE_JSON_H

#include <stddef.h>
#include "mapepire_util.h"

typedef enum {
    MPJSON_NULL = 0,
    MPJSON_BOOL,
    MPJSON_NUMBER,
    MPJSON_STRING,
    MPJSON_ARRAY,
    MPJSON_OBJECT
} mpjson_type;

typedef struct mpjson mpjson;
struct mpjson {
    mpjson_type type;
    int      boolean;
    char    *str;      /* string value (UTF-8) or number token text */
    size_t   slen;
    mpjson    **items;    /* array elements / object values */
    char   **keys;     /* object keys (parallel to items) */
    size_t   count;
    size_t   cap;
};

/* Parse; returns NULL on error and writes a message into err. */
mpjson  *mpjson_parse(const char *_text, size_t _len, char *_err, size_t _errlen);
void  mpjson_free(mpjson *_v);

/* Lookup helpers (all NULL-safe). */
mpjson        *mpjson_get(const mpjson *_obj, const char *_key);
mpjson        *mpjson_at(const mpjson *_arr, size_t _idx);
size_t      mpjson_len(const mpjson *_v);
const char *mpjson_str(const mpjson *_v);                  /* NULL unless string */
const char *mpjson_get_str(const mpjson *_obj, const char *_key);
long long   mpjson_get_int(const mpjson *_obj, const char *_key, long long _dflt);
int         mpjson_get_bool(const mpjson *_obj, const char *_key, int _dflt);
int         mpjson_is_null(const mpjson *_v);              /* true for NULL ptr too */
double      mpjson_number(const mpjson *_v);

/* Builders. */
mpjson *mpjson_new(mpjson_type _t);
mpjson *mpjson_new_string(const char *_s);
mpjson *mpjson_new_stringn(const char *_s, size_t _n);
mpjson *mpjson_new_number_text(const char *_text);
mpjson *mpjson_new_int(long long _v);
mpjson *mpjson_new_double(double _v);
mpjson *mpjson_new_bool(int _b);
int  mpjson_push(mpjson *_arr, mpjson *_v);                    /* takes ownership */
int  mpjson_set(mpjson *_obj, const char *_key, mpjson *_v);    /* takes ownership */

/* Serialize compactly into b (appends). */
int  mpjson_write(const mpjson *_v, mapepire_buf *_b);
/* Append a JSON string literal for s[0..n) to b. */
int  mpjson_write_string(mapepire_buf *_b, const char *_s, size_t _n);

#endif

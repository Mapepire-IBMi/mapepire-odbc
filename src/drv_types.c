/*
 * Db2 for i type mapping and data conversion between Mapepire JSON values
 * and ODBC C types.
 */
#include "drv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include <float.h>

#define MAPEPIRE_DEFAULT_VARLEN 32739L
#define MAPEPIRE_LOB_LEN        2147483647L

/* ---- type mapping ---------------------------------------------------------- */

static void set_char(mapepire_col *_c, SQLSMALLINT _t, long _prec, long _dflt)
{
    _c->sql_type = _t;
    _c->size = (SQLULEN)(_prec > 0 ? _prec : _dflt);
    _c->digits = 0;
    _c->display = (SQLLEN)_c->size;
    _c->octet = (SQLLEN)_c->size;
    if (_t == SQL_WCHAR || _t == SQL_WVARCHAR || _t == SQL_WLONGVARCHAR) {
        _c->octet = _c->size > (SQLULEN)(MAPEPIRE_LOB_LEN / 2) ? MAPEPIRE_LOB_LEN : (SQLLEN)_c->size * 2;
    }
    _c->case_sensitive = 1;
    _c->searchable = (_t == SQL_LONGVARCHAR || _t == SQL_WLONGVARCHAR) ? SQL_PRED_CHAR : SQL_SEARCHABLE;
    _c->radix = 0;
}

static void set_bin(mapepire_col *_c, SQLSMALLINT _t, long _prec, long _dflt)
{
    _c->sql_type = _t;
    _c->size = (SQLULEN)(_prec > 0 ? _prec : _dflt);
    _c->digits = 0;
    _c->octet = (SQLLEN)_c->size;
    _c->display = _c->size > (SQLULEN)(MAPEPIRE_LOB_LEN / 2) ? MAPEPIRE_LOB_LEN : (SQLLEN)_c->size * 2;
    _c->case_sensitive = 0;
    _c->searchable = _t == SQL_LONGVARBINARY ? SQL_PRED_NONE : SQL_SEARCHABLE;
    _c->radix = 0;
}

static void set_num(mapepire_col *_c, SQLSMALLINT _t, SQLULEN _size, SQLSMALLINT _digits, SQLLEN _display, SQLLEN _octet, int _radix)
{
    _c->sql_type = _t;
    _c->size = _size;
    _c->digits = _digits;
    _c->display = _display;
    _c->octet = _octet;
    _c->case_sensitive = 0;
    _c->searchable = SQL_PRED_BASIC;
    _c->radix = _radix;
}

int mapepire_type_describe(const char *_type_name, long _prec, long _scale, mapepire_col *_c)
{
    int known = 1;
    char t[64];
    size_t n = 0;
    for (const char *p = _type_name ? _type_name : ""; *p && n < sizeof t - 1; p++) t[n++] = (char)toupper((unsigned char)*p);
    t[n] = '\0';
    mapepire_trim(t);
    int fbd = strstr(t, "FOR BIT DATA") != NULL;
    /* strip any "(n)" / " () FOR BIT DATA" suffix to get the base name */
    char base[64];
    mapepire_snprintf(base, sizeof base, "%s", t);
    char *paren = strchr(base, '(');
    if (paren) *paren = '\0';
    char *fb = strstr(base, " FOR BIT DATA");
    if (fb) *fb = '\0';
    mapepire_trim(base);

    _c->is_unsigned = 0;
    _c->updatable = SQL_ATTR_READWRITE_UNKNOWN;

    if (!strcmp(base, "CHAR") || !strcmp(base, "CHARACTER")) {
        if (fbd) set_bin(_c, SQL_BINARY, _prec, 1);
        else set_char(_c, SQL_CHAR, _prec, 1);
    } else if (!strcmp(base, "VARCHAR") || !strcmp(base, "CHARACTER VARYING") || !strcmp(base, "CHAR VARYING") ||
               !strcmp(base, "TEXT") || !strcmp(base, "DATALINK") || !strcmp(base, "NULL") || !base[0]) {
        if (fbd) set_bin(_c, SQL_VARBINARY, _prec, MAPEPIRE_DEFAULT_VARLEN);
        else set_char(_c, SQL_VARCHAR, _prec, MAPEPIRE_DEFAULT_VARLEN);
    } else if (!strcmp(base, "LONG VARCHAR")) {
        if (fbd) set_bin(_c, SQL_LONGVARBINARY, _prec, MAPEPIRE_DEFAULT_VARLEN);
        else set_char(_c, SQL_LONGVARCHAR, _prec, MAPEPIRE_DEFAULT_VARLEN);
    } else if (!strcmp(base, "NCHAR") || !strcmp(base, "GRAPHIC") || !strcmp(base, "NATIONAL CHARACTER")) {
        set_char(_c, SQL_WCHAR, _prec, 1);
    } else if (!strcmp(base, "NVARCHAR") || !strcmp(base, "VARGRAPHIC") || !strcmp(base, "NATIONAL CHARACTER VARYING")) {
        set_char(_c, SQL_WVARCHAR, _prec, MAPEPIRE_DEFAULT_VARLEN / 2);
    } else if (!strcmp(base, "LONG VARGRAPHIC") || !strcmp(base, "DBCLOB") || !strcmp(base, "NCLOB")) {
        set_char(_c, SQL_WLONGVARCHAR, _prec, MAPEPIRE_LOB_LEN / 2);
    } else if (!strcmp(base, "CLOB") || !strcmp(base, "XML")) {
        set_char(_c, SQL_LONGVARCHAR, _prec, MAPEPIRE_LOB_LEN);
    } else if (!strcmp(base, "BINARY")) {
        set_bin(_c, SQL_BINARY, _prec, 1);
    } else if (!strcmp(base, "VARBINARY") || !strcmp(base, "BINARY VARYING")) {
        set_bin(_c, SQL_VARBINARY, _prec, MAPEPIRE_DEFAULT_VARLEN);
    } else if (!strcmp(base, "BLOB")) {
        set_bin(_c, SQL_LONGVARBINARY, _prec, MAPEPIRE_LOB_LEN);
    } else if (!strcmp(base, "ROWID")) {
        set_bin(_c, SQL_VARBINARY, _prec > 0 ? _prec : 40, 40);
    } else if (!strcmp(base, "SMALLINT")) {
        set_num(_c, SQL_SMALLINT, 5, 0, 6, 2, 10);
    } else if (!strcmp(base, "INTEGER") || !strcmp(base, "INT")) {
        set_num(_c, SQL_INTEGER, 10, 0, 11, 4, 10);
    } else if (!strcmp(base, "BIGINT")) {
        set_num(_c, SQL_BIGINT, 19, 0, 20, 8, 10);
    } else if (!strcmp(base, "DECIMAL") || !strcmp(base, "DEC") || !strcmp(base, "NUMERIC")) {
        long p = _prec > 0 ? _prec : 31;
        long s = _scale >= 0 && _scale <= p ? _scale : 0;
        set_num(_c, !strcmp(base, "NUMERIC") ? SQL_NUMERIC : SQL_DECIMAL, (SQLULEN)p, (SQLSMALLINT)s, p + 2, p + 2, 10);
    } else if (!strcmp(base, "REAL")) {
        set_num(_c, SQL_REAL, 7, 0, 14, 4, 10);
    } else if (!strcmp(base, "FLOAT") && _prec > 0 && _prec <= 24) {
        set_num(_c, SQL_REAL, 7, 0, 14, 4, 10);
    } else if (!strcmp(base, "DOUBLE") || !strcmp(base, "DOUBLE PRECISION") || !strcmp(base, "FLOAT") ||
               !strcmp(base, "FLOAT8") || !strcmp(base, "DECFLOAT")) {
        set_num(_c, SQL_DOUBLE, 15, 0, 24, 8, 10);
    } else if (!strcmp(base, "DATE")) {
        set_num(_c, SQL_TYPE_DATE, 10, 0, 10, 6, 0);
        _c->searchable = SQL_SEARCHABLE;
    } else if (!strcmp(base, "TIME")) {
        set_num(_c, SQL_TYPE_TIME, 8, 0, 8, 6, 0);
        _c->searchable = SQL_SEARCHABLE;
    } else if (!strcmp(base, "TIMESTAMP") || !strcmp(base, "TIMESTMP") || !strcmp(base, "DATETIME")) {
        long sc;
        if (_scale > 0) sc = _scale;
        else if (_prec > 20) sc = _prec - 20;
        else if (_prec == 19) sc = 0;
        else sc = 6;
        if (sc > 12) sc = 12;
        set_num(_c, SQL_TYPE_TIMESTAMP, (SQLULEN)(sc ? 20 + sc : 19), (SQLSMALLINT)sc, sc ? 20 + sc : 19, 16, 0);
        _c->searchable = SQL_SEARCHABLE;
    } else if (!strcmp(base, "BOOLEAN") || !strcmp(base, "BOOL") || !strcmp(base, "BIT")) {
        set_num(_c, SQL_BIT, 1, 0, 1, 1, 0);
        _c->is_unsigned = 1;
        _c->searchable = SQL_SEARCHABLE;
    } else {
        /* Unknown (ARRAY, distinct types, ...): expose as character data. */
        set_char(_c, SQL_VARCHAR, _prec, MAPEPIRE_DEFAULT_VARLEN);
        known = 0;
    }
    return known;
}

SQLSMALLINT mapepire_type_verbose(SQLSMALLINT _t)
{
    switch (_t) {
    case SQL_TYPE_DATE:
    case SQL_TYPE_TIME:
    case SQL_TYPE_TIMESTAMP: return SQL_DATETIME;
    default: return _t;
    }
}

SQLSMALLINT mapepire_odbc2_type(mapepire_dbc *_d, SQLSMALLINT _t)
{
    if (!_d || !_d->env || _d->env->odbc_version != SQL_OV_ODBC2) return _t;
    switch (_t) {
    case SQL_TYPE_DATE: return SQL_DATE;
    case SQL_TYPE_TIME: return SQL_TIME;
    case SQL_TYPE_TIMESTAMP: return SQL_TIMESTAMP;
    default: return _t;
    }
}

SQLSMALLINT mapepire_type_datetime_sub(SQLSMALLINT _t)
{
    switch (_t) {
    case SQL_TYPE_DATE: return SQL_CODE_DATE;
    case SQL_TYPE_TIME: return SQL_CODE_TIME;
    case SQL_TYPE_TIMESTAMP: return SQL_CODE_TIMESTAMP;
    default: return 0;
    }
}

int mapepire_sqltype_is_binary(SQLSMALLINT _t) { return _t == SQL_BINARY || _t == SQL_VARBINARY || _t == SQL_LONGVARBINARY; }

int mapepire_sqltype_is_char(SQLSMALLINT _t)
{
    return _t == SQL_CHAR || _t == SQL_VARCHAR || _t == SQL_LONGVARCHAR || _t == SQL_WCHAR || _t == SQL_WVARCHAR ||
           _t == SQL_WLONGVARCHAR;
}

int mapepire_sqltype_is_numeric(SQLSMALLINT _t)
{
    switch (_t) {
    case SQL_SMALLINT: case SQL_INTEGER: case SQL_BIGINT: case SQL_TINYINT: case SQL_DECIMAL: case SQL_NUMERIC:
    case SQL_REAL: case SQL_FLOAT: case SQL_DOUBLE: case SQL_BIT: return 1;
    default: return 0;
    }
}

int mapepire_sqltype_valid(SQLSMALLINT _t)
{
    switch (_t) {
    case SQL_CHAR: case SQL_VARCHAR: case SQL_LONGVARCHAR: case SQL_WCHAR: case SQL_WVARCHAR: case SQL_WLONGVARCHAR:
    case SQL_DECIMAL: case SQL_NUMERIC: case SQL_SMALLINT: case SQL_INTEGER: case SQL_REAL: case SQL_FLOAT:
    case SQL_DOUBLE: case SQL_BIT: case SQL_TINYINT: case SQL_BIGINT: case SQL_BINARY: case SQL_VARBINARY:
    case SQL_LONGVARBINARY: case SQL_TYPE_DATE: case SQL_TYPE_TIME: case SQL_TYPE_TIMESTAMP: case SQL_DATE:
    case SQL_TIME: case SQL_TIMESTAMP: case SQL_GUID: return 1;
    default: return 0;
    }
}

SQLSMALLINT mapepire_default_ctype(SQLSMALLINT _t)
{
    switch (_t) {
    case SQL_CHAR: case SQL_VARCHAR: case SQL_LONGVARCHAR: case SQL_DECIMAL: case SQL_NUMERIC: return SQL_C_CHAR;
    case SQL_WCHAR: case SQL_WVARCHAR: case SQL_WLONGVARCHAR: return SQL_C_WCHAR;
    case SQL_BIT: return SQL_C_BIT;
    case SQL_TINYINT: return SQL_C_STINYINT;
    case SQL_SMALLINT: return SQL_C_SSHORT;
    case SQL_INTEGER: return SQL_C_SLONG;
    case SQL_BIGINT: return SQL_C_SBIGINT;
    case SQL_REAL: return SQL_C_FLOAT;
    case SQL_FLOAT: case SQL_DOUBLE: return SQL_C_DOUBLE;
    case SQL_BINARY: case SQL_VARBINARY: case SQL_LONGVARBINARY: return SQL_C_BINARY;
    case SQL_TYPE_DATE: case SQL_DATE: return SQL_C_TYPE_DATE;
    case SQL_TYPE_TIME: case SQL_TIME: return SQL_C_TYPE_TIME;
    case SQL_TYPE_TIMESTAMP: case SQL_TIMESTAMP: return SQL_C_TYPE_TIMESTAMP;
    case SQL_GUID: return SQL_C_GUID;
    default: return SQL_C_CHAR;
    }
}

SQLLEN mapepire_ctype_size(SQLSMALLINT _ct)
{
    switch (_ct) {
    case SQL_C_STINYINT: case SQL_C_UTINYINT: case SQL_C_TINYINT: case SQL_C_BIT: return 1;
    case SQL_C_SSHORT: case SQL_C_USHORT: case SQL_C_SHORT: return sizeof(SQLSMALLINT);
    case SQL_C_SLONG: case SQL_C_ULONG: case SQL_C_LONG: return sizeof(SQLINTEGER);
    case SQL_C_SBIGINT: case SQL_C_UBIGINT: return sizeof(SQLBIGINT);
    case SQL_C_FLOAT: return sizeof(SQLREAL);
    case SQL_C_DOUBLE: return sizeof(SQLDOUBLE);
    case SQL_C_NUMERIC: return sizeof(SQL_NUMERIC_STRUCT);
    case SQL_C_TYPE_DATE: case SQL_C_DATE: return sizeof(SQL_DATE_STRUCT);
    case SQL_C_TYPE_TIME: case SQL_C_TIME: return sizeof(SQL_TIME_STRUCT);
    case SQL_C_TYPE_TIMESTAMP: case SQL_C_TIMESTAMP: return sizeof(SQL_TIMESTAMP_STRUCT);
    case SQL_C_GUID: return sizeof(SQLGUID);
    default: return 0;
    }
}

int mapepire_ctype_valid(SQLSMALLINT _ct)
{
    switch (_ct) {
    case SQL_C_CHAR: case SQL_C_WCHAR: case SQL_C_BINARY: case SQL_C_DEFAULT:
        return 1;
    default:
        return mapepire_ctype_size(_ct) != 0;
    }
}

/* ---- numbers --------------------------------------------------------------- */

/*
 * Accepts [ws][+-]digits[.digits][(e|E)[+-]digits][ws] (or .digits) and
 * writes plain notation without exponent, redundant '+' or leading zeros.
 */
int mapepire_number_normalize(const char *_in, char *_out, size_t _cap)
{
    const char *p = _in;
    while (*p == ' ' || *p == '\t') p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    const char *ip = p;
    while (isdigit((unsigned char)*p)) p++;
    size_t ilen = (size_t)(p - ip);
    const char *fp = NULL;
    size_t flen = 0;
    if (*p == '.') {
        p++;
        fp = p;
        while (isdigit((unsigned char)*p)) p++;
        flen = (size_t)(p - fp);
    }
    if (!ilen && !flen) return -1;
    long exp = 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        int eneg = 0;
        if (*p == '+' || *p == '-') { eneg = *p == '-'; p++; }
        if (!isdigit((unsigned char)*p)) return -1;
        while (isdigit((unsigned char)*p)) {
            if (exp < 100000) exp = exp * 10 + (*p - '0');
            p++;
        }
        if (eneg) exp = -exp;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) return -1;
    if (exp > 4000 || exp < -4000) return -1;

    /* digit string D with the decimal point after `point` digits */
    size_t dlen = ilen + flen;
    char *d = (char *)malloc(dlen + 1);
    if (!d) return -1;
    memcpy(d, ip, ilen);
    if (flen) memcpy(d + ilen, fp, flen);
    d[dlen] = '\0';
    long point = (long)ilen + exp;
    /* drop leading zeros of the integer part, keeping one before the point */
    size_t lead = 0;
    while (lead + 1 < dlen && d[lead] == '0' && (long)lead + 1 < point) lead++;
    const char *D = d + lead;
    dlen -= lead;
    point -= (long)lead;

    mapepire_buf b;
    mapepire_buf_init(&b);
    if (neg) mapepire_buf_append_char(&b, '-');
    if (point <= 0) {
        mapepire_buf_append_str(&b, "0.");
        for (long i = 0; i < -point; i++) mapepire_buf_append_char(&b, '0');
        mapepire_buf_append(&b, D, dlen);
    } else if ((size_t)point >= dlen) {
        mapepire_buf_append(&b, D, dlen);
        for (long i = (long)dlen; i < point; i++) mapepire_buf_append_char(&b, '0');
    } else {
        mapepire_buf_append(&b, D, (size_t)point);
        mapepire_buf_append_char(&b, '.');
        mapepire_buf_append(&b, D + point, dlen - (size_t)point);
    }
    free(d);
    int rc = 0;
    if (!b.data || b.len + 1 > _cap) rc = -1;
    else memcpy(_out, b.data, b.len + 1);
    mapepire_buf_free(&b);
    return rc;
}

/* Split a normalized number into sign, integer digits and fraction digits. */
typedef struct {
    int         neg;
    const char *ip;
    size_t      ilen;
    const char *fp;
    size_t      flen;
} numparts;

static void split_number(const char *_norm, numparts *_np)
{
    const char *p = _norm;
    _np->neg = *p == '-';
    if (_np->neg) p++;
    _np->ip = p;
    while (isdigit((unsigned char)*p)) p++;
    _np->ilen = (size_t)(p - _np->ip);
    _np->fp = "";
    _np->flen = 0;
    if (*p == '.') {
        _np->fp = p + 1;
        _np->flen = strlen(_np->fp);
    }
}

static int frac_nonzero(const numparts *_np)
{
    for (size_t i = 0; i < _np->flen; i++)
        if (_np->fp[i] != '0') return 1;
    return 0;
}

/* Parse the integer part as magnitude; returns -1 on overflow. */
static int int_magnitude(const numparts *_np, unsigned long long *_mag)
{
    unsigned long long v = 0;
    for (size_t i = 0; i < _np->ilen; i++) {
        unsigned d = (unsigned)(_np->ip[i] - '0');
        if (v > (ULLONG_MAX - d) / 10) return -1;
        v = v * 10 + d;
    }
    *_mag = v;
    return 0;
}

/* ---- date / time parsing ------------------------------------------------------ */

static int digits_n(const char *_s, int _n, int *_out)
{
    int v = 0;
    for (int i = 0; i < _n; i++) {
        if (!isdigit((unsigned char)_s[i])) return -1;
        v = v * 10 + (_s[i] - '0');
    }
    *_out = v;
    return 0;
}

static int parse_time_part(const char *_p, SQL_TIMESTAMP_STRUCT *_ts, const char **_end)
{
    int h, m, sec = 0;
    if (digits_n(_p, 2, &h)) {
        /* allow a single-digit hour */
        if (isdigit((unsigned char)_p[0]) && (_p[1] == ':' || _p[1] == '.')) { h = _p[0] - '0'; _p -= 1; }
        else return -1;
    }
    if (_p[2] != ':' && _p[2] != '.') return -1;
    if (digits_n(_p + 3, 2, &m)) return -1;
    _p += 5;
    if (*_p == ':' || *_p == '.') {
        if (digits_n(_p + 1, 2, &sec)) return -1;
        _p += 3;
    }
    unsigned long frac = 0;
    if (*_p == '.' || *_p == ',') {
        _p++;
        int nd = 0;
        while (isdigit((unsigned char)*_p)) {
            if (nd < 9) { frac = frac * 10 + (unsigned long)(*_p - '0'); nd++; }
            _p++;
        }
        while (nd < 9) { frac *= 10; nd++; }
    }
    if (h > 24 || m > 59 || sec > 60) return -1;
    _ts->hour = (SQLUSMALLINT)h;
    _ts->minute = (SQLUSMALLINT)m;
    _ts->second = (SQLUSMALLINT)sec;
    _ts->fraction = (SQLUINTEGER)frac;
    *_end = _p;
    return 0;
}

/* Parses ISO / Db2 date, time and timestamp text. */
static int parse_datetime(const char *_s, SQL_TIMESTAMP_STRUCT *_ts, int *_has_date, int *_has_time)
{
    memset(_ts, 0, sizeof *_ts);
    *_has_date = *_has_time = 0;
    while (*_s == ' ') _s++;
    const char *p = _s;
    int y, mo, d;
    if (!digits_n(p, 4, &y) && (p[4] == '-' || p[4] == '/') && !digits_n(p + 5, 2, &mo) && p[7] == p[4] &&
        !digits_n(p + 8, 2, &d)) {
        if (mo < 1 || mo > 12 || d < 1 || d > 31) return -1;
        _ts->year = (SQLSMALLINT)y;
        _ts->month = (SQLUSMALLINT)mo;
        _ts->day = (SQLUSMALLINT)d;
        *_has_date = 1;
        p += 10;
        if (*p == ' ' || *p == 'T' || *p == '-') {
            const char *e;
            if (parse_time_part(p + 1, _ts, &e)) return -1;
            *_has_time = 1;
            p = e;
        }
    } else {
        const char *e;
        if (parse_time_part(p, _ts, &e)) return -1;
        *_has_time = 1;
        p = e;
    }
    while (*p == ' ') p++;
    /* tolerate a trailing zone designator */
    if (*p == 'Z') p++;
    return *p ? -1 : 0;
}

/* ---- binary decoding ---------------------------------------------------------- */

static int hexv(int _c)
{
    if (_c >= '0' && _c <= '9') return _c - '0';
    if (_c >= 'a' && _c <= 'f') return _c - 'a' + 10;
    if (_c >= 'A' && _c <= 'F') return _c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *_s, size_t _n, mapepire_buf *_out)
{
    if (_n % 2) return -1;
    for (size_t i = 0; i < _n; i++)
        if (hexv((unsigned char)_s[i]) < 0) return -1;
    mapepire_buf_reserve(_out, _n / 2);
    for (size_t i = 0; i < _n; i += 2) {
        char b = (char)(hexv((unsigned char)_s[i]) << 4 | hexv((unsigned char)_s[i + 1]));
        mapepire_buf_append_char(_out, b);
    }
    return 0;
}

static int b64v(int _c)
{
    if (_c >= 'A' && _c <= 'Z') return _c - 'A';
    if (_c >= 'a' && _c <= 'z') return _c - 'a' + 26;
    if (_c >= '0' && _c <= '9') return _c - '0' + 52;
    if (_c == '+' || _c == '-') return 62;
    if (_c == '/' || _c == '_') return 63;
    return -1;
}

static int base64_decode(const char *_s, size_t _n, mapepire_buf *_out)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t i = 0;
    for (; i < _n; i++) {
        int c = (unsigned char)_s[i];
        if (c == '=' ) break;
        if (c == '\r' || c == '\n' || c == ' ') continue;
        int v = b64v(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            mapepire_buf_append_char(_out, (char)((acc >> bits) & 0xFF));
        }
    }
    for (; i < _n; i++)
        if (_s[i] != '=' && _s[i] != '\r' && _s[i] != '\n' && _s[i] != ' ') return -1;
    return 0;
}

static void hex_encode(const char *_s, size_t _n, mapepire_buf *_out)
{
    static const char hx[] = "0123456789ABCDEF";
    mapepire_buf_reserve(_out, _n * 2);
    for (size_t i = 0; i < _n; i++) {
        mapepire_buf_append_char(_out, hx[(unsigned char)_s[i] >> 4]);
        mapepire_buf_append_char(_out, hx[(unsigned char)_s[i] & 15]);
    }
}

/* ---- cell rendering ----------------------------------------------------------- */

/*
 * Produce the canonical representation of a cell: raw bytes for binary
 * columns (is_bin = 1), otherwise UTF-8 text.
 */
static SQLRETURN render_cell(mapepire_stmt *_s, const mapepire_col *_col, const mpjson *_cell, mapepire_buf *_out, int *_is_bin)
{
    *_is_bin = mapepire_sqltype_is_binary(_col->sql_type);
    mapepire_buf_clear(_out);
    switch (_cell->type) {
    case MPJSON_STRING:
        if (*_is_bin) {
            mapepire_buf tmp;
            mapepire_buf_init(&tmp);
            if (!hex_decode(_cell->str, _cell->slen, &tmp) || (mapepire_buf_clear(&tmp), !base64_decode(_cell->str, _cell->slen, &tmp))) {
                mapepire_buf_append(_out, tmp.data ? tmp.data : "", tmp.len);
            } else {
                mapepire_buf_append(_out, _cell->str, _cell->slen);
            }
            mapepire_buf_free(&tmp);
        } else {
            mapepire_buf_append(_out, _cell->str, _cell->slen);
        }
        break;
    case MPJSON_NUMBER: {
        if (strpbrk(_cell->str, "eE") &&
            (_col->sql_type == SQL_DECIMAL || _col->sql_type == SQL_NUMERIC || _col->sql_type == SQL_INTEGER ||
             _col->sql_type == SQL_SMALLINT || _col->sql_type == SQL_BIGINT)) {
            char norm[512];
            if (!mapepire_number_normalize(_cell->str, norm, sizeof norm)) {
                mapepire_buf_append_str(_out, norm);
                break;
            }
        }
        mapepire_buf_append(_out, _cell->str, _cell->slen);
        break;
    }
    case MPJSON_BOOL:
        if (_col->sql_type == SQL_BIT || mapepire_sqltype_is_numeric(_col->sql_type)) mapepire_buf_append_str(_out, _cell->boolean ? "1" : "0");
        else mapepire_buf_append_str(_out, _cell->boolean ? "true" : "false");
        break;
    case MPJSON_OBJECT: {
        const char *url = mpjson_get_str(_cell, "blob_url");
        if (url) {
            mapepire_error e;
            memset(&e, 0, sizeof e);
            if (mapepire_client_http_get(_s->dbc->cli, url, _out, &e)) return mapepire_diag_from_error(&_s->h, &e);
            if (!*_is_bin) { /* character LOB delivered as bytes: already UTF-8 */ }
            break;
        }
        mpjson_write(_cell, _out);
        break;
    }
    case MPJSON_ARRAY:
        mpjson_write(_cell, _out);
        break;
    default:
        break;
    }
    if (!_out->data) mapepire_buf_append(_out, "", 0);
    return SQL_SUCCESS;
}

/* Produce the bytes the application receives for a variable-length C type. */
static SQLRETURN render_for_ctype(mapepire_stmt *_s, const mapepire_col *_col, const mpjson *_cell, SQLSMALLINT _ctype, mapepire_buf *_out)
{
    mapepire_buf canon;
    mapepire_buf_init(&canon);
    int is_bin;
    SQLRETURN rc = render_cell(_s, _col, _cell, &canon, &is_bin);
    if (!SQL_SUCCEEDED(rc)) { mapepire_buf_free(&canon); return rc; }
    mapepire_buf_clear(_out);
    if (_ctype == SQL_C_BINARY) {
        mapepire_buf_append(_out, canon.data, canon.len);
    } else {
        mapepire_buf text;
        mapepire_buf_init(&text);
        if (is_bin) hex_encode(canon.data, canon.len, &text);
        else mapepire_buf_append(&text, canon.data, canon.len);
        if (!text.data) mapepire_buf_append(&text, "", 0);
        if (_ctype == SQL_C_WCHAR) {
            size_t units = mapepire_utf8_to_utf16(text.data, text.len, NULL, 0);
            mapepire_buf_reserve(_out, (units + 1) * sizeof(SQLWCHAR));
            mapepire_utf8_to_utf16(text.data, text.len, (uint16_t *)_out->data, units + 1);
            _out->len = units * sizeof(SQLWCHAR);
        } else {
            mapepire_dbc_utf8_to_char(_s->dbc, text.data, text.len, _out);
        }
        mapepire_buf_free(&text);
    }
    if (!_out->data) mapepire_buf_append(_out, "", 0);
    mapepire_buf_free(&canon);
    return SQL_SUCCESS;
}

/* ---- cell -> C ------------------------------------------------------------------ */

static SQLRETURN warn(mapepire_stmt *_s, SQLRETURN _rc, const char *_state, SQLLEN _row, SQLINTEGER _col, const char *_msg)
{
    mapepire_diag_add_rc(&_s->h, _state, 0, _row, _col, "%s", _msg);
    return _rc == SQL_ERROR ? SQL_ERROR : SQL_SUCCESS_WITH_INFO;
}

static SQLRETURN err(mapepire_stmt *_s, const char *_state, SQLLEN _row, SQLINTEGER _col, const char *_msg)
{
    mapepire_diag_add_rc(&_s->h, _state, 0, _row, _col, "%s", _msg);
    return SQL_ERROR;
}

static SQLRETURN copy_chunk(mapepire_stmt *_s, SQLSMALLINT _ctype, const mapepire_buf *_src, size_t *_off, SQLPOINTER _target,
                            SQLLEN _buflen, SQLLEN *_ind, SQLLEN _row, SQLINTEGER _colno, int *_finished)
{
    size_t total = _src->len;
    size_t remain = total - *_off;
    size_t term = _ctype == SQL_C_CHAR ? 1 : _ctype == SQL_C_WCHAR ? sizeof(SQLWCHAR) : 0;
    if (_ind) *_ind = (SQLLEN)remain;
    SQLRETURN rc = SQL_SUCCESS;
    if (!_target || _buflen < 0) {
        if (_target && _buflen < 0) return err(_s, "HY090", _row, _colno, "Invalid buffer length");
        /* only the length was requested; nothing is consumed */
        return SQL_SUCCESS;
    }
    size_t room = (size_t)_buflen >= term ? (size_t)_buflen - term : 0;
    if (_ctype == SQL_C_WCHAR) room -= room % sizeof(SQLWCHAR);
    size_t n = remain < room ? remain : room;
    if (n) memcpy(_target, _src->data + *_off, n);
    if (term && (size_t)_buflen >= term) memset((char *)_target + n, 0, term);
    *_off += n;
    if (n < remain) {
        rc = warn(_s, rc, "01004", _row, _colno, "String data, right truncated");
    } else if (_finished) {
        *_finished = 1;
    }
    return rc;
}

SQLRETURN mapepire_cell_to_c(mapepire_stmt *_s, const mapepire_col *_col, const mpjson *_cell, SQLSMALLINT _ctype, SQLPOINTER _target,
                       SQLLEN _buflen, SQLLEN *_ind, SQLLEN _row, SQLINTEGER _colno, mapepire_getdata *_gd)
{
    if (_ctype == SQL_C_DEFAULT) _ctype = mapepire_default_ctype(_col->sql_type);
    if (_gd && *_gd->finished) return SQL_NO_DATA;

    if (mpjson_is_null(_cell)) {
        if (!_ind) return err(_s, "22002", _row, _colno, "Indicator variable required but not supplied");
        *_ind = SQL_NULL_DATA;
        if (_gd) *_gd->finished = 1;
        return SQL_SUCCESS;
    }

    /* variable-length targets */
    if (_ctype == SQL_C_CHAR || _ctype == SQL_C_WCHAR || _ctype == SQL_C_BINARY) {
        if (_gd) {
            if (!*_gd->started) {
                SQLRETURN rc = render_for_ctype(_s, _col, _cell, _ctype, _gd->cache);
                if (!SQL_SUCCEEDED(rc)) return rc;
                *_gd->off = 0;
                *_gd->started = 1;
            }
            return copy_chunk(_s, _ctype, _gd->cache, _gd->off, _target, _buflen, _ind, _row, _colno, _gd->finished);
        }
        mapepire_buf tmp;
        mapepire_buf_init(&tmp);
        SQLRETURN rc = render_for_ctype(_s, _col, _cell, _ctype, &tmp);
        if (SQL_SUCCEEDED(rc)) {
            size_t off = 0;
            rc = copy_chunk(_s, _ctype, &tmp, &off, _target, _buflen, _ind, _row, _colno, NULL);
        }
        mapepire_buf_free(&tmp);
        return rc;
    }

    /* fixed-size targets */
    if (_gd) *_gd->finished = 1;
    SQLLEN fsize = mapepire_ctype_size(_ctype);
    if (!fsize) return err(_s, "HY003", _row, _colno, "Program type out of range");
    if (!_target) {
        /* SQLBindCol may bind only an indicator; SQLGetData needs a buffer */
        if (_gd) return err(_s, "HY009", _row, _colno, "Invalid use of null pointer");
        if (_ind) *_ind = fsize;
        return SQL_SUCCESS;
    }

    mapepire_buf text;
    mapepire_buf_init(&text);
    int is_bin = 0;
    SQLRETURN rc = render_cell(_s, _col, _cell, &text, &is_bin);
    if (!SQL_SUCCEEDED(rc)) { mapepire_buf_free(&text); return rc; }
    const char *t = text.data ? text.data : "";
    int is_dt_col = _col->sql_type == SQL_TYPE_DATE || _col->sql_type == SQL_TYPE_TIME || _col->sql_type == SQL_TYPE_TIMESTAMP;
    rc = SQL_SUCCESS;

    switch (_ctype) {
    case SQL_C_STINYINT: case SQL_C_TINYINT: case SQL_C_UTINYINT: case SQL_C_SSHORT: case SQL_C_SHORT:
    case SQL_C_USHORT: case SQL_C_SLONG: case SQL_C_LONG: case SQL_C_ULONG: case SQL_C_SBIGINT: case SQL_C_UBIGINT:
    case SQL_C_BIT: {
        if (is_bin || is_dt_col) { rc = err(_s, "07006", _row, _colno, "Restricted data type attribute violation"); break; }
        char norm[512];
        const char *src = t;
        if (!mapepire_stricmp(t, "true")) src = "1";
        else if (!mapepire_stricmp(t, "false")) src = "0";
        if (mapepire_number_normalize(src, norm, sizeof norm)) {
            /* fall back to strtod for things like "1.0E400" */
            rc = err(_s, "22018", _row, _colno, "Invalid character value for cast specification");
            break;
        }
        numparts np;
        split_number(norm, &np);
        unsigned long long mag;
        int over = int_magnitude(&np, &mag);
        int truncated = frac_nonzero(&np);
        long long sv = 0;
        unsigned long long uv = 0;
        int is_signed = _ctype == SQL_C_STINYINT || _ctype == SQL_C_TINYINT || _ctype == SQL_C_SSHORT ||
                        _ctype == SQL_C_SHORT || _ctype == SQL_C_SLONG || _ctype == SQL_C_LONG || _ctype == SQL_C_SBIGINT;
        long long lo = 0;
        unsigned long long hi = 0;
        switch (_ctype) {
        case SQL_C_STINYINT: case SQL_C_TINYINT: lo = SCHAR_MIN; hi = SCHAR_MAX; break;
        case SQL_C_UTINYINT: hi = UCHAR_MAX; break;
        case SQL_C_BIT: hi = 1; break;
        case SQL_C_SSHORT: case SQL_C_SHORT: lo = SHRT_MIN; hi = SHRT_MAX; break;
        case SQL_C_USHORT: hi = USHRT_MAX; break;
        case SQL_C_SLONG: case SQL_C_LONG: lo = INT_MIN; hi = INT_MAX; break;
        case SQL_C_ULONG: hi = UINT_MAX; break;
        case SQL_C_SBIGINT: lo = LLONG_MIN; hi = LLONG_MAX; break;
        case SQL_C_UBIGINT: hi = ULLONG_MAX; break;
        }
        int range_err = over;
        if (!range_err) {
            if (np.neg && mag != 0) {
                if (!is_signed) range_err = 1;
                else if (mag > (unsigned long long)LLONG_MAX + 1ULL || -(long long)(mag - 1) - 1 < lo) range_err = 1;
                else sv = -(long long)(mag - 1) - 1;
            } else {
                if (mag > hi) range_err = 1;
                else { uv = mag; sv = (long long)mag; }
            }
        }
        if (_ctype == SQL_C_BIT && !range_err && truncated && mag >= 1) range_err = 1; /* 1.x -> out of range */
        if (range_err) { rc = err(_s, "22003", _row, _colno, "Numeric value out of range"); break; }
        switch (_ctype) {
        case SQL_C_STINYINT: case SQL_C_TINYINT: *(SQLSCHAR *)_target = (SQLSCHAR)sv; break;
        case SQL_C_UTINYINT: case SQL_C_BIT: *(SQLCHAR *)_target = (SQLCHAR)uv; break;
        case SQL_C_SSHORT: case SQL_C_SHORT: *(SQLSMALLINT *)_target = (SQLSMALLINT)sv; break;
        case SQL_C_USHORT: *(SQLUSMALLINT *)_target = (SQLUSMALLINT)uv; break;
        case SQL_C_SLONG: case SQL_C_LONG: *(SQLINTEGER *)_target = (SQLINTEGER)sv; break;
        case SQL_C_ULONG: *(SQLUINTEGER *)_target = (SQLUINTEGER)uv; break;
        case SQL_C_SBIGINT: *(SQLBIGINT *)_target = (SQLBIGINT)sv; break;
        case SQL_C_UBIGINT: *(SQLUBIGINT *)_target = (SQLUBIGINT)uv; break;
        }
        if (truncated) rc = warn(_s, rc, "01S07", _row, _colno, "Fractional truncation");
        break;
    }
    case SQL_C_FLOAT:
    case SQL_C_DOUBLE: {
        if (is_bin || is_dt_col) { rc = err(_s, "07006", _row, _colno, "Restricted data type attribute violation"); break; }
        const char *src = t;
        if (!mapepire_stricmp(t, "true")) src = "1";
        else if (!mapepire_stricmp(t, "false")) src = "0";
        char *end;
        while (*src == ' ') src++;
        double v = strtod(src, &end);
        while (*end == ' ') end++;
        if (end == src || *end) { rc = err(_s, "22018", _row, _colno, "Invalid character value for cast specification"); break; }
        if (_ctype == SQL_C_FLOAT) {
            if (isfinite(v) && fabs(v) > FLT_MAX) { rc = err(_s, "22003", _row, _colno, "Numeric value out of range"); break; }
            *(SQLREAL *)_target = (SQLREAL)v;
        } else {
            *(SQLDOUBLE *)_target = v;
        }
        break;
    }
    case SQL_C_NUMERIC: {
        if (is_bin || is_dt_col) { rc = err(_s, "07006", _row, _colno, "Restricted data type attribute violation"); break; }
        char norm[512];
        if (mapepire_number_normalize(t, norm, sizeof norm)) { rc = err(_s, "22018", _row, _colno, "Invalid character value for cast specification"); break; }
        numparts np;
        split_number(norm, &np);
        int scale = _col->digits < 0 ? 0 : _col->digits > 38 ? 38 : _col->digits;
        SQL_NUMERIC_STRUCT *ns = (SQL_NUMERIC_STRUCT *)_target;
        memset(ns, 0, sizeof *ns);
        ns->precision = (SQLCHAR)(_col->size > 0 && _col->size <= 38 ? _col->size : 38);
        ns->scale = (SQLSCHAR)scale;
        ns->sign = np.neg ? 0 : 1;
        int truncated = 0;
        /* accumulate digits of int part + scale fraction digits into a 128-bit LE number */
        uint32_t w[4] = {0, 0, 0, 0};
        int overflow = 0;
        size_t total = np.ilen + (size_t)scale;
        for (size_t i = 0; i < total && !overflow; i++) {
            unsigned dgt;
            if (i < np.ilen) dgt = (unsigned)(np.ip[i] - '0');
            else {
                size_t fi = i - np.ilen;
                dgt = fi < np.flen ? (unsigned)(np.fp[fi] - '0') : 0;
            }
            uint64_t carry = dgt;
            for (int k = 0; k < 4; k++) {
                uint64_t v = (uint64_t)w[k] * 10 + carry;
                w[k] = (uint32_t)v;
                carry = v >> 32;
            }
            if (carry) overflow = 1;
        }
        for (size_t fi = (size_t)scale; fi < np.flen; fi++)
            if (np.fp[fi] != '0') truncated = 1;
        if (overflow) { rc = err(_s, "22003", _row, _colno, "Numeric value out of range"); break; }
        for (int k = 0; k < 4; k++)
            for (int b = 0; b < 4; b++) ns->val[k * 4 + b] = (SQLCHAR)(w[k] >> (8 * b));
        if (truncated) rc = warn(_s, rc, "01S07", _row, _colno, "Fractional truncation");
        break;
    }
    case SQL_C_TYPE_DATE: case SQL_C_DATE: case SQL_C_TYPE_TIME: case SQL_C_TIME: case SQL_C_TYPE_TIMESTAMP:
    case SQL_C_TIMESTAMP: {
        if (is_bin || (mapepire_sqltype_is_numeric(_col->sql_type))) {
            rc = err(_s, "07006", _row, _colno, "Restricted data type attribute violation");
            break;
        }
        SQL_TIMESTAMP_STRUCT ts;
        int hd, ht;
        if (parse_datetime(t, &ts, &hd, &ht)) {
            rc = err(_s, "22018", _row, _colno, "Invalid character value for cast specification");
            break;
        }
        if (_ctype == SQL_C_TYPE_DATE || _ctype == SQL_C_DATE) {
            if (!hd) { rc = err(_s, "07006", _row, _colno, "Restricted data type attribute violation"); break; }
            SQL_DATE_STRUCT *ds = (SQL_DATE_STRUCT *)_target;
            ds->year = ts.year;
            ds->month = ts.month;
            ds->day = ts.day;
            if (ht && (ts.hour || ts.minute || ts.second || ts.fraction))
                rc = warn(_s, rc, "01S07", _row, _colno, "Fractional truncation");
        } else if (_ctype == SQL_C_TYPE_TIME || _ctype == SQL_C_TIME) {
            if (!ht) { rc = err(_s, "07006", _row, _colno, "Restricted data type attribute violation"); break; }
            SQL_TIME_STRUCT *tm = (SQL_TIME_STRUCT *)_target;
            tm->hour = ts.hour;
            tm->minute = ts.minute;
            tm->second = ts.second;
            if (ts.fraction) rc = warn(_s, rc, "01S07", _row, _colno, "Fractional truncation");
        } else {
            if (!hd) {
                time_t now = time(NULL);
                struct tm lt;
#ifdef _WIN32
                localtime_s(&lt, &now);
#else
                localtime_r(&now, &lt);
#endif
                ts.year = (SQLSMALLINT)(lt.tm_year + 1900);
                ts.month = (SQLUSMALLINT)(lt.tm_mon + 1);
                ts.day = (SQLUSMALLINT)lt.tm_mday;
            }
            *(SQL_TIMESTAMP_STRUCT *)_target = ts;
        }
        break;
    }
    case SQL_C_GUID:
    default:
        rc = err(_s, "07006", _row, _colno, "Restricted data type attribute violation");
        break;
    }
    mapepire_buf_free(&text);
    if (SQL_SUCCEEDED(rc) && _ind) *_ind = fsize;
    return rc;
}

/* ---- parameters -> JSON ---------------------------------------------------------- */

static size_t wlen16(const SQLWCHAR *_w, size_t _max_units)
{
    size_t n = 0;
    while (n < _max_units && _w[n]) n++;
    return n;
}

/* Is the text a valid JSON number token after trimming? Returns trimmed copy or NULL. */
static char *as_json_number(const char *_s)
{
    while (*_s == ' ') _s++;
    size_t n = strlen(_s);
    while (n && _s[n - 1] == ' ') n--;
    char *c = mapepire_strndup(_s, n);
    if (!c) return NULL;
    char err[8];
    mpjson *v = mpjson_parse(c, n, err, sizeof err);
    int ok = v && v->type == MPJSON_NUMBER;
    mpjson_free(v);
    if (!ok) {
        /* accept leading '+' or leading zeros by normalizing */
        char norm[512];
        if (!mapepire_number_normalize(c, norm, sizeof norm)) {
            free(c);
            return mapepire_strdup(norm);
        }
        free(c);
        return NULL;
    }
    return c;
}

static mpjson *text_value(const char *_utf8, size_t _len, SQLSMALLINT _sqltype)
{
    if (mapepire_sqltype_is_numeric(_sqltype) && _sqltype != SQL_BIT) {
        char *tmp = mapepire_strndup(_utf8, _len);
        char *num = tmp ? as_json_number(tmp) : NULL;
        free(tmp);
        if (num) {
            mpjson *v = mpjson_new_number_text(num);
            free(num);
            return v;
        }
    }
    return mpjson_new_stringn(_utf8, _len);
}

static void numeric_to_text(const SQL_NUMERIC_STRUCT *_ns, char *_out, size_t _cap)
{
    /* 128-bit little endian magnitude -> decimal digits */
    uint32_t w[4];
    for (int k = 0; k < 4; k++)
        w[k] = (uint32_t)_ns->val[k * 4] | (uint32_t)_ns->val[k * 4 + 1] << 8 | (uint32_t)_ns->val[k * 4 + 2] << 16 |
               (uint32_t)_ns->val[k * 4 + 3] << 24;
    char digits[64];
    int nd = 0;
    for (;;) {
        int zero = !w[0] && !w[1] && !w[2] && !w[3];
        if (zero) break;
        uint64_t rem = 0;
        for (int k = 3; k >= 0; k--) {
            uint64_t cur = (rem << 32) | w[k];
            w[k] = (uint32_t)(cur / 10);
            rem = cur % 10;
        }
        digits[nd++] = (char)('0' + rem);
    }
    if (!nd) digits[nd++] = '0';
    /* digits are reversed */
    mapepire_buf b;
    mapepire_buf_init(&b);
    int scale = _ns->scale;
    if (_ns->sign == 0 && !(nd == 1 && digits[0] == '0')) mapepire_buf_append_char(&b, '-');
    if (scale <= 0) {
        for (int i = nd - 1; i >= 0; i--) mapepire_buf_append_char(&b, digits[i]);
        for (int i = 0; i < -scale; i++) mapepire_buf_append_char(&b, '0');
    } else {
        if (nd <= scale) {
            mapepire_buf_append_str(&b, "0.");
            for (int i = 0; i < scale - nd; i++) mapepire_buf_append_char(&b, '0');
            for (int i = nd - 1; i >= 0; i--) mapepire_buf_append_char(&b, digits[i]);
        } else {
            for (int i = nd - 1; i >= scale; i--) mapepire_buf_append_char(&b, digits[i]);
            mapepire_buf_append_char(&b, '.');
            for (int i = scale - 1; i >= 0; i--) mapepire_buf_append_char(&b, digits[i]);
        }
    }
    mapepire_snprintf(_out, _cap, "%s", b.data ? b.data : "0");
    mapepire_buf_free(&b);
}

static void format_fraction(char *_out, size_t _cap, SQLUINTEGER _frac)
{
    if (_frac % 1000) mapepire_snprintf(_out, _cap, ".%09lu", (unsigned long)_frac);
    else mapepire_snprintf(_out, _cap, ".%06lu", (unsigned long)(_frac / 1000));
}

SQLRETURN mapepire_param_to_json(mapepire_stmt *_s, int _pidx, SQLULEN _row, mpjson **_out)
{
    mapepire_parambind *pb = &_s->pbind[_pidx];
    *_out = NULL;
    SQLINTEGER pno = _pidx + 1;
    SQLSMALLINT ctype = pb->ctype == SQL_C_DEFAULT ? mapepire_default_ctype(pb->sqltype) : pb->ctype;
    SQLLEN offset = _s->param_bind_offset ? (SQLLEN)*_s->param_bind_offset : 0;
    char *base = (char *)pb->ptr;
    SQLLEN *ind = pb->ind;
    SQLLEN elem = mapepire_ctype_size(ctype);
    if (!elem) elem = pb->buflen;
    if (_s->param_bind_type == SQL_PARAM_BIND_BY_COLUMN) {
        if (base) base += offset + (SQLLEN)_row * elem;
        if (ind) ind = (SQLLEN *)((char *)ind + offset) + _row;
    } else {
        SQLLEN stride = (SQLLEN)_s->param_bind_type;
        if (base) base += offset + (SQLLEN)_row * stride;
        if (ind) ind = (SQLLEN *)((char *)ind + offset + (SQLLEN)_row * stride);
    }
    SQLLEN indv = ind ? *ind : SQL_NTS;
    if (pb->io == SQL_PARAM_OUTPUT) { *_out = mpjson_new(MPJSON_NULL); return SQL_SUCCESS; }

    /* data supplied with SQLPutData */
    if (pb->dae && mapepire_ctype_size(ctype) && !pb->dae_null) {
        /* fixed-size value: convert it like a bound buffer */
        if (pb->dae_data.len < (size_t)mapepire_ctype_size(ctype)) {
            mapepire_diag_add_rc(&_s->h, "22026", 0, (SQLLEN)_row + 1, pno, "String data, length mismatch");
            return SQL_ERROR;
        }
        base = pb->dae_data.data;
        ind = NULL;
        indv = 0;
    } else if (pb->dae) {
        if (pb->dae_null) { *_out = mpjson_new(MPJSON_NULL); return SQL_SUCCESS; }
        const char *d = pb->dae_data.data ? pb->dae_data.data : "";
        size_t n = pb->dae_data.len;
        if (ctype == SQL_C_WCHAR) {
            char *u = mapepire_utf16_to_utf8((const uint16_t *)d, (long)(n / sizeof(SQLWCHAR)), &n);
            *_out = text_value(u, n, pb->sqltype);
            free(u);
        } else if (ctype == SQL_C_BINARY) {
            if (mapepire_sqltype_is_char(pb->sqltype)) {
                size_t ul;
                char *u = mapepire_utf8_sanitize(d, n, &ul);
                *_out = mpjson_new_stringn(u ? u : "", u ? ul : 0);
                free(u);
            } else {
                char *b64 = mapepire_base64_encode(d, n);
                *_out = mpjson_new_string(b64);
                free(b64);
            }
        } else {
            size_t ul;
            char *u = mapepire_dbc_char_to_utf8(_s->dbc, d, n, &ul);
            *_out = text_value(u, ul, pb->sqltype);
            free(u);
        }
        return *_out ? SQL_SUCCESS : SQL_ERROR;
    }

    if (indv == SQL_NULL_DATA) { *_out = mpjson_new(MPJSON_NULL); return SQL_SUCCESS; }
    if (!base) {
        mapepire_diag_add_rc(&_s->h, "07002", 0, (SQLLEN)_row + 1, pno, "No data pointer bound for parameter %d", (int)pno);
        return SQL_ERROR;
    }

    char tmp[128];
    switch (ctype) {
    case SQL_C_CHAR: {
        if (ind && indv < 0 && indv != SQL_NTS) {
            mapepire_diag_add_rc(&_s->h, "HY090", 0, (SQLLEN)_row + 1, pno, "Invalid string or buffer length");
            return SQL_ERROR;
        }
        size_t n = (indv == SQL_NTS || !ind) ? strlen(base) : (size_t)indv;
        size_t ul;
        char *u = mapepire_dbc_char_to_utf8(_s->dbc, base, n, &ul);
        *_out = text_value(u ? u : "", u ? ul : 0, pb->sqltype);
        free(u);
        break;
    }
    case SQL_C_WCHAR: {
        if (ind && indv < 0 && indv != SQL_NTS) {
            mapepire_diag_add_rc(&_s->h, "HY090", 0, (SQLLEN)_row + 1, pno, "Invalid string or buffer length");
            return SQL_ERROR;
        }
        size_t units = (indv == SQL_NTS || !ind) ? wlen16((const SQLWCHAR *)base, (size_t)-1 / 4)
                                                   : (size_t)indv / sizeof(SQLWCHAR);
        size_t ul;
        char *u = mapepire_utf16_to_utf8((const uint16_t *)base, (long)units, &ul);
        *_out = text_value(u ? u : "", u ? ul : 0, pb->sqltype);
        free(u);
        break;
    }
    case SQL_C_BINARY: {
        size_t n = ind ? (size_t)(indv < 0 ? 0 : indv) : (size_t)(pb->buflen > 0 ? pb->buflen : 0);
        if (mapepire_sqltype_is_char(pb->sqltype)) {
            size_t ul;
            char *u = mapepire_utf8_sanitize(base, n, &ul);
            *_out = mpjson_new_stringn(u ? u : "", u ? ul : 0);
            free(u);
        } else {
            char *b64 = mapepire_base64_encode(base, n);
            *_out = mpjson_new_string(b64 ? b64 : "");
            free(b64);
        }
        break;
    }
    case SQL_C_STINYINT: case SQL_C_TINYINT: *_out = mpjson_new_int(*(SQLSCHAR *)base); break;
    case SQL_C_UTINYINT: *_out = mpjson_new_int(*(SQLCHAR *)base); break;
    case SQL_C_BIT: *_out = mpjson_new_int(*(SQLCHAR *)base ? 1 : 0); break;
    case SQL_C_SSHORT: case SQL_C_SHORT: *_out = mpjson_new_int(*(SQLSMALLINT *)base); break;
    case SQL_C_USHORT: *_out = mpjson_new_int(*(SQLUSMALLINT *)base); break;
    case SQL_C_SLONG: case SQL_C_LONG: *_out = mpjson_new_int(*(SQLINTEGER *)base); break;
    case SQL_C_ULONG: *_out = mpjson_new_int((long long)*(SQLUINTEGER *)base); break;
    case SQL_C_SBIGINT: *_out = mpjson_new_int((long long)*(SQLBIGINT *)base); break;
    case SQL_C_UBIGINT:
        snprintf(tmp, sizeof tmp, "%llu", (unsigned long long)*(SQLUBIGINT *)base);
        *_out = mpjson_new_number_text(tmp);
        break;
    case SQL_C_FLOAT:
    case SQL_C_DOUBLE: {
        double v = ctype == SQL_C_FLOAT ? (double)*(SQLREAL *)base : *(SQLDOUBLE *)base;
        if (!isfinite(v)) {
            mapepire_diag_add_rc(&_s->h, "22003", 0, (SQLLEN)_row + 1, pno, "Numeric value out of range");
            return SQL_ERROR;
        }
        snprintf(tmp, sizeof tmp, ctype == SQL_C_FLOAT ? "%.9g" : "%.17g", v);
        *_out = mpjson_new_number_text(tmp);
        break;
    }
    case SQL_C_NUMERIC:
        numeric_to_text((SQL_NUMERIC_STRUCT *)base, tmp, sizeof tmp);
        *_out = mpjson_new_number_text(tmp);
        break;
    case SQL_C_TYPE_DATE: case SQL_C_DATE: {
        SQL_DATE_STRUCT *d = (SQL_DATE_STRUCT *)base;
        snprintf(tmp, sizeof tmp, "%04d-%02u-%02u", d->year, d->month, d->day);
        *_out = mpjson_new_string(tmp);
        break;
    }
    case SQL_C_TYPE_TIME: case SQL_C_TIME: {
        SQL_TIME_STRUCT *t = (SQL_TIME_STRUCT *)base;
        snprintf(tmp, sizeof tmp, "%02u:%02u:%02u", t->hour, t->minute, t->second);
        *_out = mpjson_new_string(tmp);
        break;
    }
    case SQL_C_TYPE_TIMESTAMP: case SQL_C_TIMESTAMP: {
        SQL_TIMESTAMP_STRUCT *t = (SQL_TIMESTAMP_STRUCT *)base;
        char fr[16];
        format_fraction(fr, sizeof fr, t->fraction);
        if (pb->sqltype == SQL_TYPE_DATE || pb->sqltype == SQL_DATE)
            snprintf(tmp, sizeof tmp, "%04d-%02u-%02u", t->year, t->month, t->day);
        else if (pb->sqltype == SQL_TYPE_TIME || pb->sqltype == SQL_TIME)
            snprintf(tmp, sizeof tmp, "%02u:%02u:%02u", t->hour, t->minute, t->second);
        else
            snprintf(tmp, sizeof tmp, "%04d-%02u-%02u %02u:%02u:%02u%s", t->year, t->month, t->day, t->hour, t->minute,
                     t->second, fr);
        *_out = mpjson_new_string(tmp);
        break;
    }
    case SQL_C_GUID: {
        SQLGUID *g = (SQLGUID *)base;
        snprintf(tmp, sizeof tmp, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned long)g->Data1,
                 g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5],
                 g->Data4[6], g->Data4[7]);
        *_out = mpjson_new_string(tmp);
        break;
    }
    default:
        mapepire_diag_add_rc(&_s->h, "HY003", 0, (SQLLEN)_row + 1, pno, "Program type out of range");
        return SQL_ERROR;
    }
    if (!*_out) {
        mapepire_diag_add(&_s->h, "HY001", 0, "Out of memory");
        return SQL_ERROR;
    }
    return SQL_SUCCESS;
}

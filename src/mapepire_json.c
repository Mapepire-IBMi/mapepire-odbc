#include "mapepire_json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MPJSON_MAX_DEPTH 256

typedef struct {
    const char *p;
    const char *end;
    char       *err;
    size_t      errlen;
} parser;

static void perr(parser *_ps, const char *_msg)
{
    if (_ps->err && _ps->errlen && !_ps->err[0])
        mapepire_snprintf(_ps->err, _ps->errlen, "JSON parse error: %s", _msg);
}

static void skip_ws(parser *_ps)
{
    while (_ps->p < _ps->end && (*_ps->p == ' ' || *_ps->p == '\t' || *_ps->p == '\n' || *_ps->p == '\r'))
        _ps->p++;
}

mpjson *mpjson_new(mpjson_type _t)
{
    mpjson *v = (mpjson *)calloc(1, sizeof(mpjson));
    if (v) v->type = _t;
    return v;
}

void mpjson_free(mpjson *_v)
{
    if (!_v) return;
    for (size_t i = 0; i < _v->count; i++) {
        mpjson_free(_v->items[i]);
        if (_v->keys) free(_v->keys[i]);
    }
    free(_v->items);
    free(_v->keys);
    free(_v->str);
    free(_v);
}

static int grow(mpjson *_v, int _with_keys)
{
    if (_v->count < _v->cap) return 0;
    size_t ncap = _v->cap ? _v->cap * 2 : 8;
    mpjson **ni = (mpjson **)realloc(_v->items, ncap * sizeof(mpjson *));
    if (!ni) return -1;
    _v->items = ni;
    if (_with_keys) {
        char **nk = (char **)realloc(_v->keys, ncap * sizeof(char *));
        if (!nk) return -1;
        _v->keys = nk;
    }
    _v->cap = ncap;
    return 0;
}

static int hexval(char _c)
{
    if (_c >= '0' && _c <= '9') return _c - '0';
    if (_c >= 'a' && _c <= 'f') return _c - 'a' + 10;
    if (_c >= 'A' && _c <= 'F') return _c - 'A' + 10;
    return -1;
}

static int read_hex4(parser *_ps, unsigned *_out)
{
    if (_ps->end - _ps->p < 4) return -1;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hexval(_ps->p[i]);
        if (h < 0) return -1;
        v = (v << 4) | (unsigned)h;
    }
    _ps->p += 4;
    *_out = v;
    return 0;
}

static void put_utf8(mapepire_buf *_b, unsigned _cp)
{
    char t[4];
    if (_cp < 0x80) { t[0] = (char)_cp; mapepire_buf_append(_b, t, 1); }
    else if (_cp < 0x800) {
        t[0] = (char)(0xC0 | (_cp >> 6)); t[1] = (char)(0x80 | (_cp & 0x3F));
        mapepire_buf_append(_b, t, 2);
    } else if (_cp < 0x10000) {
        t[0] = (char)(0xE0 | (_cp >> 12)); t[1] = (char)(0x80 | ((_cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (_cp & 0x3F));
        mapepire_buf_append(_b, t, 3);
    } else {
        t[0] = (char)(0xF0 | (_cp >> 18)); t[1] = (char)(0x80 | ((_cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((_cp >> 6) & 0x3F)); t[3] = (char)(0x80 | (_cp & 0x3F));
        mapepire_buf_append(_b, t, 4);
    }
}

/* Parses a string literal starting at the opening quote. */
static int parse_string_raw(parser *_ps, char **_out, size_t *_outlen)
{
    mapepire_buf b;
    mapepire_buf_init(&b);
    _ps->p++; /* opening quote */
    for (;;) {
        if (_ps->p >= _ps->end) { perr(_ps, "unterminated string"); goto fail; }
        /* copy a run of plain characters at once */
        const char *run = _ps->p;
        while (_ps->p < _ps->end && *_ps->p != '"' && *_ps->p != '\\' && (unsigned char)*_ps->p >= 0x20) _ps->p++;
        if (_ps->p > run && mapepire_buf_append(&b, run, (size_t)(_ps->p - run))) goto oom;
        if (_ps->p >= _ps->end) { perr(_ps, "unterminated string"); goto fail; }
        char c = *_ps->p;
        if (c == '"') { _ps->p++; break; }
        if ((unsigned char)c < 0x20) { perr(_ps, "control character in string"); goto fail; }
        /* backslash escape */
        _ps->p++;
        if (_ps->p >= _ps->end) { perr(_ps, "bad escape"); goto fail; }
        c = *_ps->p++;
        switch (c) {
        case '"': mapepire_buf_append_char(&b, '"'); break;
        case '\\': mapepire_buf_append_char(&b, '\\'); break;
        case '/': mapepire_buf_append_char(&b, '/'); break;
        case 'b': mapepire_buf_append_char(&b, '\b'); break;
        case 'f': mapepire_buf_append_char(&b, '\f'); break;
        case 'n': mapepire_buf_append_char(&b, '\n'); break;
        case 'r': mapepire_buf_append_char(&b, '\r'); break;
        case 't': mapepire_buf_append_char(&b, '\t'); break;
        case 'u': {
            unsigned cp;
            if (read_hex4(_ps, &cp)) { perr(_ps, "bad \\u escape"); goto fail; }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (_ps->end - _ps->p >= 6 && _ps->p[0] == '\\' && _ps->p[1] == 'u') {
                    const char *save = _ps->p;
                    _ps->p += 2;
                    if (!read_hex4(_ps, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        _ps->p = save;
                        cp = 0xFFFD;
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            put_utf8(&b, cp);
            break;
        }
        default: perr(_ps, "bad escape"); goto fail;
        }
    }
    *_outlen = b.len;
    *_out = mapepire_buf_detach(&b);
    return 0;
oom:
    perr(_ps, "out of memory");
fail:
    mapepire_buf_free(&b);
    return -1;
}

static mpjson *parse_value(parser *_ps, int _depth);

static mpjson *parse_number(parser *_ps)
{
    const char *s = _ps->p;
    if (_ps->p < _ps->end && *_ps->p == '-') _ps->p++;
    if (_ps->p >= _ps->end || !(*_ps->p >= '0' && *_ps->p <= '9')) { perr(_ps, "bad number"); return NULL; }
    if (*_ps->p == '0' && _ps->p + 1 < _ps->end && _ps->p[1] >= '0' && _ps->p[1] <= '9') {
        perr(_ps, "leading zeros are not allowed");
        return NULL;
    }
    while (_ps->p < _ps->end && *_ps->p >= '0' && *_ps->p <= '9') _ps->p++;
    if (_ps->p < _ps->end && *_ps->p == '.') {
        _ps->p++;
        if (_ps->p >= _ps->end || !(*_ps->p >= '0' && *_ps->p <= '9')) { perr(_ps, "bad number"); return NULL; }
        while (_ps->p < _ps->end && *_ps->p >= '0' && *_ps->p <= '9') _ps->p++;
    }
    if (_ps->p < _ps->end && (*_ps->p == 'e' || *_ps->p == 'E')) {
        _ps->p++;
        if (_ps->p < _ps->end && (*_ps->p == '+' || *_ps->p == '-')) _ps->p++;
        if (_ps->p >= _ps->end || !(*_ps->p >= '0' && *_ps->p <= '9')) { perr(_ps, "bad number"); return NULL; }
        while (_ps->p < _ps->end && *_ps->p >= '0' && *_ps->p <= '9') _ps->p++;
    }
    mpjson *v = mpjson_new(MPJSON_NUMBER);
    if (!v) return NULL;
    v->slen = (size_t)(_ps->p - s);
    v->str = mapepire_strndup(s, v->slen);
    if (!v->str) { free(v); return NULL; }
    return v;
}

static int match_lit(parser *_ps, const char *_lit)
{
    size_t n = strlen(_lit);
    if ((size_t)(_ps->end - _ps->p) < n || memcmp(_ps->p, _lit, n)) return 0;
    _ps->p += n;
    return 1;
}

static mpjson *parse_value(parser *_ps, int _depth)
{
    if (_depth > MPJSON_MAX_DEPTH) { perr(_ps, "nesting too deep"); return NULL; }
    skip_ws(_ps);
    if (_ps->p >= _ps->end) { perr(_ps, "unexpected end of input"); return NULL; }
    char c = *_ps->p;
    if (c == '{') {
        mpjson *o = mpjson_new(MPJSON_OBJECT);
        if (!o) return NULL;
        _ps->p++;
        skip_ws(_ps);
        if (_ps->p < _ps->end && *_ps->p == '}') { _ps->p++; return o; }
        for (;;) {
            skip_ws(_ps);
            if (_ps->p >= _ps->end || *_ps->p != '"') { perr(_ps, "expected object key"); goto ofail; }
            char *key; size_t klen;
            if (parse_string_raw(_ps, &key, &klen)) goto ofail;
            skip_ws(_ps);
            if (_ps->p >= _ps->end || *_ps->p != ':') { free(key); perr(_ps, "expected ':'"); goto ofail; }
            _ps->p++;
            mpjson *val = parse_value(_ps, _depth + 1);
            if (!val) { free(key); goto ofail; }
            if (grow(o, 1)) { free(key); mpjson_free(val); goto ofail; }
            o->keys[o->count] = key;
            o->items[o->count++] = val;
            skip_ws(_ps);
            if (_ps->p < _ps->end && *_ps->p == ',') { _ps->p++; continue; }
            if (_ps->p < _ps->end && *_ps->p == '}') { _ps->p++; return o; }
            perr(_ps, "expected ',' or '}'");
            goto ofail;
        }
    ofail:
        mpjson_free(o);
        return NULL;
    }
    if (c == '[') {
        mpjson *a = mpjson_new(MPJSON_ARRAY);
        if (!a) return NULL;
        _ps->p++;
        skip_ws(_ps);
        if (_ps->p < _ps->end && *_ps->p == ']') { _ps->p++; return a; }
        for (;;) {
            mpjson *val = parse_value(_ps, _depth + 1);
            if (!val) goto afail;
            if (grow(a, 0)) { mpjson_free(val); goto afail; }
            a->items[a->count++] = val;
            skip_ws(_ps);
            if (_ps->p < _ps->end && *_ps->p == ',') { _ps->p++; continue; }
            if (_ps->p < _ps->end && *_ps->p == ']') { _ps->p++; return a; }
            perr(_ps, "expected ',' or ']'");
            goto afail;
        }
    afail:
        mpjson_free(a);
        return NULL;
    }
    if (c == '"') {
        mpjson *s = mpjson_new(MPJSON_STRING);
        if (!s) return NULL;
        if (parse_string_raw(_ps, &s->str, &s->slen)) { free(s); return NULL; }
        return s;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(_ps);
    if (match_lit(_ps, "true")) { mpjson *b = mpjson_new(MPJSON_BOOL); if (b) b->boolean = 1; return b; }
    if (match_lit(_ps, "false")) return mpjson_new(MPJSON_BOOL);
    if (match_lit(_ps, "null")) return mpjson_new(MPJSON_NULL);
    perr(_ps, "unexpected character");
    return NULL;
}

mpjson *mpjson_parse(const char *_text, size_t _len, char *_err, size_t _errlen)
{
    parser ps = {_text, _text + _len, _err, _errlen};
    if (_err && _errlen) _err[0] = '\0';
    mpjson *v = parse_value(&ps, 0);
    if (!v) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) { perr(&ps, "trailing characters"); mpjson_free(v); return NULL; }
    return v;
}

/* ---- accessors ------------------------------------------------------------ */

mpjson *mpjson_get(const mpjson *_obj, const char *_key)
{
    if (!_obj || _obj->type != MPJSON_OBJECT) return NULL;
    for (size_t i = 0; i < _obj->count; i++)
        if (!strcmp(_obj->keys[i], _key)) return _obj->items[i];
    return NULL;
}

mpjson *mpjson_at(const mpjson *_arr, size_t _idx)
{
    if (!_arr || (_arr->type != MPJSON_ARRAY && _arr->type != MPJSON_OBJECT) || _idx >= _arr->count) return NULL;
    return _arr->items[_idx];
}

size_t mpjson_len(const mpjson *_v) { return _v && (_v->type == MPJSON_ARRAY || _v->type == MPJSON_OBJECT) ? _v->count : 0; }

const char *mpjson_str(const mpjson *_v) { return _v && _v->type == MPJSON_STRING ? _v->str : NULL; }

const char *mpjson_get_str(const mpjson *_obj, const char *_key) { return mpjson_str(mpjson_get(_obj, _key)); }

long long mpjson_get_int(const mpjson *_obj, const char *_key, long long _dflt)
{
    mpjson *v = mpjson_get(_obj, _key);
    if (!v) return _dflt;
    if (v->type == MPJSON_NUMBER) return (long long)strtod(v->str, NULL);
    if (v->type == MPJSON_STRING) return strtoll(v->str, NULL, 10);
    if (v->type == MPJSON_BOOL) return v->boolean;
    return _dflt;
}

int mpjson_get_bool(const mpjson *_obj, const char *_key, int _dflt)
{
    mpjson *v = mpjson_get(_obj, _key);
    if (!v) return _dflt;
    if (v->type == MPJSON_BOOL) return v->boolean;
    if (v->type == MPJSON_NUMBER) return strtod(v->str, NULL) != 0;
    if (v->type == MPJSON_STRING) return mapepire_parse_bool(v->str, _dflt);
    return _dflt;
}

int mpjson_is_null(const mpjson *_v) { return !_v || _v->type == MPJSON_NULL; }

double mpjson_number(const mpjson *_v)
{
    if (!_v) return 0;
    if (_v->type == MPJSON_NUMBER || _v->type == MPJSON_STRING) return strtod(_v->str, NULL);
    if (_v->type == MPJSON_BOOL) return _v->boolean;
    return 0;
}

/* ---- builders ------------------------------------------------------------- */

mpjson *mpjson_new_stringn(const char *_s, size_t _n)
{
    mpjson *v = mpjson_new(MPJSON_STRING);
    if (!v) return NULL;
    v->str = mapepire_strndup(_s ? _s : "", _s ? _n : 0);
    v->slen = _s ? _n : 0;
    if (!v->str) { free(v); return NULL; }
    return v;
}

mpjson *mpjson_new_string(const char *_s) { return mpjson_new_stringn(_s, _s ? strlen(_s) : 0); }

mpjson *mpjson_new_number_text(const char *_text)
{
    mpjson *v = mpjson_new(MPJSON_NUMBER);
    if (!v) return NULL;
    v->str = mapepire_strdup(_text);
    v->slen = strlen(_text);
    return v;
}

mpjson *mpjson_new_int(long long _x)
{
    char t[32];
    snprintf(t, sizeof t, "%lld", _x);
    return mpjson_new_number_text(t);
}

mpjson *mpjson_new_double(double _x)
{
    char t[40];
    snprintf(t, sizeof t, "%.17g", _x);
    /* JSON has no NaN/Infinity; callers should avoid them, but stay valid JSON. */
    if (strstr(t, "nan") || strstr(t, "inf")) return mpjson_new(MPJSON_NULL);
    return mpjson_new_number_text(t);
}

mpjson *mpjson_new_bool(int _b)
{
    mpjson *v = mpjson_new(MPJSON_BOOL);
    if (v) v->boolean = _b ? 1 : 0;
    return v;
}

int mpjson_push(mpjson *_arr, mpjson *_v)
{
    if (!_arr || !_v || grow(_arr, 0)) { mpjson_free(_v); return -1; }
    _arr->items[_arr->count++] = _v;
    return 0;
}

int mpjson_set(mpjson *_obj, const char *_key, mpjson *_v)
{
    if (!_obj || !_v) { mpjson_free(_v); return -1; }
    for (size_t i = 0; i < _obj->count; i++) {
        if (!strcmp(_obj->keys[i], _key)) {
            mpjson_free(_obj->items[i]);
            _obj->items[i] = _v;
            return 0;
        }
    }
    if (grow(_obj, 1)) { mpjson_free(_v); return -1; }
    _obj->keys[_obj->count] = mapepire_strdup(_key);
    _obj->items[_obj->count++] = _v;
    return 0;
}

/* ---- writer --------------------------------------------------------------- */

int mpjson_write_string(mapepire_buf *_b, const char *_s, size_t _n)
{
    static const char hex[] = "0123456789abcdef";
    if (mapepire_buf_reserve(_b, _n + 2)) return -1;
    mapepire_buf_append_char(_b, '"');
    size_t run = 0;
    for (size_t i = 0; i < _n; i++) {
        unsigned char c = (unsigned char)_s[i];
        const char *esc = NULL;
        char u[7];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c == '\n') esc = "\\n";
        else if (c == '\r') esc = "\\r";
        else if (c == '\t') esc = "\\t";
        else if (c == '\b') esc = "\\b";
        else if (c == '\f') esc = "\\f";
        else if (c < 0x20) {
            u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
            u[4] = hex[c >> 4]; u[5] = hex[c & 15]; u[6] = 0;
            esc = u;
        }
        if (esc) {
            if (i > run) mapepire_buf_append(_b, _s + run, i - run);
            mapepire_buf_append_str(_b, esc);
            run = i + 1;
        }
    }
    if (_n > run) mapepire_buf_append(_b, _s + run, _n - run);
    return mapepire_buf_append_char(_b, '"');
}

int mpjson_write(const mpjson *_v, mapepire_buf *_b)
{
    if (!_v) return mapepire_buf_append_str(_b, "null");
    switch (_v->type) {
    case MPJSON_NULL: return mapepire_buf_append_str(_b, "null");
    case MPJSON_BOOL: return mapepire_buf_append_str(_b, _v->boolean ? "true" : "false");
    case MPJSON_NUMBER: return mapepire_buf_append(_b, _v->str, _v->slen);
    case MPJSON_STRING: return mpjson_write_string(_b, _v->str, _v->slen);
    case MPJSON_ARRAY:
        mapepire_buf_append_char(_b, '[');
        for (size_t i = 0; i < _v->count; i++) {
            if (i) mapepire_buf_append_char(_b, ',');
            if (mpjson_write(_v->items[i], _b)) return -1;
        }
        return mapepire_buf_append_char(_b, ']');
    case MPJSON_OBJECT:
        mapepire_buf_append_char(_b, '{');
        for (size_t i = 0; i < _v->count; i++) {
            if (i) mapepire_buf_append_char(_b, ',');
            mpjson_write_string(_b, _v->keys[i], strlen(_v->keys[i]));
            mapepire_buf_append_char(_b, ':');
            if (mpjson_write(_v->items[i], _b)) return -1;
        }
        return mapepire_buf_append_char(_b, '}');
    }
    return -1;
}

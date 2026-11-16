/*
 * Diagnostic records, UTF-16 string marshalling, and SQLGetDiagRecW /
 * SQLGetDiagFieldW.
 */
#include "drv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- string helpers ------------------------------------------------------ */

char *mapepire_from_w(const SQLWCHAR *_s, SQLINTEGER _len)
{
    if (!_s) return NULL;
    if (_len == SQL_NTS || _len < 0) return mapepire_utf16_to_utf8((const uint16_t *)_s, -1, NULL);
    return mapepire_utf16_to_utf8((const uint16_t *)_s, (long)_len, NULL);
}

/*
 * Write s as UTF-16 into buf (cap units).  Returns the units the whole value
 * needs; *trunc is set when buf is non-NULL and the value did not fit.
 */
static size_t put_w(const char *_s, SQLWCHAR *_buf, long _cap, int *_trunc)
{
    if (!_s) _s = "";
    size_t len = strlen(_s);
    size_t ucap = (_buf && _cap > 0) ? (size_t)_cap : 0;
    size_t need = mapepire_utf8_to_utf16(_s, len, ucap ? (uint16_t *)_buf : NULL, ucap);
    *_trunc = _buf != NULL && (ucap == 0 ? need > 0 : need >= ucap);
    return need;
}

int mapepire_to_w_chars(const char *_s, SQLWCHAR *_buf, SQLSMALLINT _cap, SQLSMALLINT *_outlen)
{
    int trunc;
    size_t need = put_w(_s, _buf, _cap, &trunc);
    if (_outlen) *_outlen = (SQLSMALLINT)(need > 32767 ? 32767 : need);
    return trunc;
}

int mapepire_to_w_bytes(const char *_s, SQLPOINTER _buf, SQLINTEGER _cap, SQLINTEGER *_outlen)
{
    int trunc;
    size_t need = put_w(_s, (SQLWCHAR *)_buf, _cap > 0 ? (long)(_cap / (SQLINTEGER)sizeof(SQLWCHAR)) : 0, &trunc);
    if (_outlen) *_outlen = (SQLINTEGER)(need * sizeof(SQLWCHAR));
    return trunc;
}

int mapepire_to_w_bytes_small(const char *_s, SQLPOINTER _buf, SQLSMALLINT _cap, SQLSMALLINT *_outlen)
{
    SQLINTEGER l = 0;
    int t = mapepire_to_w_bytes(_s, _buf, _cap, &l);
    if (_outlen) *_outlen = (SQLSMALLINT)(l > 32767 ? 32767 : l);
    return t;
}

/* ---- diagnostics --------------------------------------------------------- */

void mapepire_diag_free(mapepire_diag *_d)
{
    for (int i = 0; i < _d->count; i++) free(_d->recs[i].msg);
    free(_d->recs);
    _d->recs = NULL;
    _d->count = _d->cap = 0;
    _d->rc = SQL_SUCCESS;
}

void mapepire_diag_clear(mapepire_handle *_h)
{
    for (int i = 0; i < _h->diag.count; i++) free(_h->diag.recs[i].msg);
    _h->diag.count = 0;
    _h->diag.rc = SQL_SUCCESS;
}

static void add_rec(mapepire_handle *_h, const char *_state, SQLINTEGER _native, SQLLEN _row, SQLINTEGER _col, const char *_msg)
{
    mapepire_diag *d = &_h->diag;
    if (d->count == d->cap) {
        int ncap = d->cap ? d->cap * 2 : 4;
        mapepire_diag_rec *nr = (mapepire_diag_rec *)realloc(d->recs, (size_t)ncap * sizeof *nr);
        if (!nr) return;
        d->recs = nr;
        d->cap = ncap;
    }
    mapepire_diag_rec *r = &d->recs[d->count++];
    mapepire_snprintf(r->state, sizeof r->state, "%s", _state && strlen(_state) == 5 ? _state : "HY000");
    r->native = _native;
    r->msg = mapepire_strdup(_msg);
    r->row = _row;
    r->col = _col;
}

void mapepire_diag_add_rc(mapepire_handle *_h, const char *_state, SQLINTEGER _native, SQLLEN _row, SQLINTEGER _col, const char *_fmt,
                    ...)
{
    char body[1200];
    va_list ap;
    va_start(ap, _fmt);
    vsnprintf(body, sizeof body, _fmt, ap);
    va_end(ap);
    body[sizeof body - 1] = '\0';
    char msg[1300];
    mapepire_snprintf(msg, sizeof msg, "%s%s", MAPEPIRE_MSG_PREFIX, body);
    add_rec(_h, _state, _native, _row, _col, msg);
}

void mapepire_diag_add(mapepire_handle *_h, const char *_state, SQLINTEGER _native, const char *_fmt, ...)
{
    char body[1200];
    va_list ap;
    va_start(ap, _fmt);
    vsnprintf(body, sizeof body, _fmt, ap);
    va_end(ap);
    body[sizeof body - 1] = '\0';
    char msg[1300];
    mapepire_snprintf(msg, sizeof msg, "%s%s", MAPEPIRE_MSG_PREFIX, body);
    add_rec(_h, _state, _native, SQL_NO_ROW_NUMBER, SQL_NO_COLUMN_NUMBER, msg);
}

SQLRETURN mapepire_diag_from_error(mapepire_handle *_h, const mapepire_error *_e)
{
    const char *state = _e->sqlstate[0] ? _e->sqlstate : NULL;
    if (!state) {
        switch (_e->kind) {
        case MAPEPIRE_ERR_CONNECT:
        case MAPEPIRE_ERR_TLS: state = "08001"; break;
        case MAPEPIRE_ERR_AUTH: state = "28000"; break;
        case MAPEPIRE_ERR_COMM:
        case MAPEPIRE_ERR_PROTOCOL: state = "08S01"; break;
        case MAPEPIRE_ERR_TIMEOUT: state = "HYT00"; break;
        case MAPEPIRE_ERR_MEMORY: state = "HY001"; break;
        default: state = "HY000"; break;
        }
    }
    char msg[1300];
    mapepire_snprintf(msg, sizeof msg, "%s%s", _e->kind == MAPEPIRE_ERR_SERVER ? MAPEPIRE_SRV_PREFIX : MAPEPIRE_MSG_PREFIX, _e->msg);
    add_rec(_h, state, (SQLINTEGER)_e->native, SQL_NO_ROW_NUMBER, SQL_NO_COLUMN_NUMBER, msg);
    return SQL_ERROR;
}

SQLRETURN mapepire_ret(mapepire_handle *_h, SQLRETURN _rc)
{
    _h->diag.rc = _rc;
    return _rc;
}

static mapepire_handle *handle_of(SQLSMALLINT _type, SQLHANDLE _handle)
{
    if (!_handle) return NULL;
    mapepire_handle *h = (mapepire_handle *)_handle;
    switch (_type) {
    case SQL_HANDLE_ENV: return h->magic == MAPEPIRE_H_ENV ? h : NULL;
    case SQL_HANDLE_DBC: return h->magic == MAPEPIRE_H_DBC ? h : NULL;
    case SQL_HANDLE_STMT: return h->magic == MAPEPIRE_H_STMT ? h : NULL;
    case SQL_HANDLE_DESC: return h->magic == MAPEPIRE_H_DESC ? h : NULL;
    default: return NULL;
    }
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetDiagRecW(SQLSMALLINT _handle_type, SQLHANDLE _handle, SQLSMALLINT _rec_number,
                                           SQLWCHAR *_sql_state, SQLINTEGER *_native_error, SQLWCHAR *_message_text,
                                           SQLSMALLINT _buffer_length, SQLSMALLINT *_text_length)
{
    mapepire_handle *h = handle_of(_handle_type, _handle);
    if (!h) return SQL_INVALID_HANDLE;
    if (_rec_number <= 0) return SQL_ERROR;
    if (_buffer_length < 0) return SQL_ERROR;
    if (_rec_number > h->diag.count) return SQL_NO_DATA;
    mapepire_diag_rec *r = &h->diag.recs[_rec_number - 1];
    if (_sql_state) mapepire_utf8_to_utf16(r->state, 5, (uint16_t *)_sql_state, 6);
    if (_native_error) *_native_error = r->native;
    int trunc = mapepire_to_w_chars(r->msg, _message_text, _buffer_length, _text_length);
    return trunc && _message_text ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

static const char *class_origin(const char *_state)
{
    /* ODBC-specific classes come from "ODBC 3.0"; everything else is ISO 9075 */
    if (!strncmp(_state, "IM", 2) || !strncmp(_state, "HY", 2) || !strcmp(_state, "01S00") ||
        !strcmp(_state, "01S01") || !strcmp(_state, "01S02") || !strcmp(_state, "01S06") ||
        !strcmp(_state, "01S07") || !strcmp(_state, "07S01") || !strcmp(_state, "08S01") ||
        !strncmp(_state, "21S", 3) || !strncmp(_state, "25S", 3) || !strncmp(_state, "42S", 3))
        return "ODBC 3.0";
    return "ISO 9075";
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetDiagFieldW(SQLSMALLINT _handle_type, SQLHANDLE _handle, SQLSMALLINT _rec_number,
                                             SQLSMALLINT _diag_identifier, SQLPOINTER _diag_info,
                                             SQLSMALLINT _buffer_length, SQLSMALLINT *_string_length)
{
    mapepire_handle *h = handle_of(_handle_type, _handle);
    if (!h) return SQL_INVALID_HANDLE;

    /* header fields */
    switch (_diag_identifier) {
    case SQL_DIAG_NUMBER:
        if (_diag_info) *(SQLINTEGER *)_diag_info = h->diag.count;
        return SQL_SUCCESS;
    case SQL_DIAG_RETURNCODE:
        if (_diag_info) *(SQLRETURN *)_diag_info = h->diag.rc;
        return SQL_SUCCESS;
    case SQL_DIAG_ROW_COUNT:
    case SQL_DIAG_CURSOR_ROW_COUNT:
        if (_handle_type != SQL_HANDLE_STMT) return SQL_ERROR;
        if (_diag_info) *(SQLLEN *)_diag_info = _diag_identifier == SQL_DIAG_ROW_COUNT ? ((mapepire_stmt *)h)->row_count : -1;
        return SQL_SUCCESS;
    case SQL_DIAG_DYNAMIC_FUNCTION:
        if (_handle_type != SQL_HANDLE_STMT) return SQL_ERROR;
        return mapepire_to_w_bytes_small("", _diag_info, _buffer_length, _string_length) ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    case SQL_DIAG_DYNAMIC_FUNCTION_CODE:
        if (_handle_type != SQL_HANDLE_STMT) return SQL_ERROR;
        if (_diag_info) *(SQLINTEGER *)_diag_info = SQL_DIAG_UNKNOWN_STATEMENT;
        return SQL_SUCCESS;
    default: break;
    }

    if (_rec_number <= 0) return SQL_ERROR;
    if (_rec_number > h->diag.count) return SQL_NO_DATA;
    mapepire_diag_rec *r = &h->diag.recs[_rec_number - 1];
    const char *s = NULL;
    switch (_diag_identifier) {
    case SQL_DIAG_SQLSTATE: s = r->state; break;
    case SQL_DIAG_MESSAGE_TEXT: s = r->msg; break;
    case SQL_DIAG_CLASS_ORIGIN:
    case SQL_DIAG_SUBCLASS_ORIGIN: s = class_origin(r->state); break;
    case SQL_DIAG_CONNECTION_NAME: s = ""; break;
    case SQL_DIAG_SERVER_NAME: {
        mapepire_dbc *d = NULL;
        if (_handle_type == SQL_HANDLE_DBC) d = (mapepire_dbc *)h;
        else if (_handle_type == SQL_HANDLE_STMT) d = ((mapepire_stmt *)h)->dbc;
        s = d && d->server ? d->server : "";
        break;
    }
    case SQL_DIAG_NATIVE:
        if (_diag_info) *(SQLINTEGER *)_diag_info = r->native;
        return SQL_SUCCESS;
    case SQL_DIAG_ROW_NUMBER:
        if (_handle_type != SQL_HANDLE_STMT) return SQL_ERROR;
        if (_diag_info) *(SQLLEN *)_diag_info = r->row;
        return SQL_SUCCESS;
    case SQL_DIAG_COLUMN_NUMBER:
        if (_handle_type != SQL_HANDLE_STMT) return SQL_ERROR;
        if (_diag_info) *(SQLINTEGER *)_diag_info = r->col;
        return SQL_SUCCESS;
    default: return SQL_ERROR;
    }
    return mapepire_to_w_bytes_small(s, _diag_info, _buffer_length, _string_length) && _diag_info ? SQL_SUCCESS_WITH_INFO
                                                                                   : SQL_SUCCESS;
}

/*
 * Connecting, disconnecting, connection attributes and transactions.
 */
#include "drv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

void mapepire_stmt_destroy(mapepire_stmt *_s);

int mapepire_dbc_connected(mapepire_dbc *_d) { return _d->cli != NULL; }

/* ---- character encoding of SQL_C_CHAR data ---------------------------------- */

char *mapepire_dbc_char_to_utf8(mapepire_dbc *_d, const char *_s, size_t _len, size_t *_outlen)
{
#ifdef _WIN32
    if (_d->char_encoding == MAPEPIRE_CHARENC_ANSI && _len) {
        int wn = MultiByteToWideChar(CP_ACP, 0, _s, (int)_len, NULL, 0);
        WCHAR *w = (WCHAR *)malloc(((size_t)wn + 1) * sizeof(WCHAR));
        if (!w) return NULL;
        MultiByteToWideChar(CP_ACP, 0, _s, (int)_len, w, wn);
        char *u = mapepire_utf16_to_utf8((const uint16_t *)w, wn, _outlen);
        free(w);
        return u;
    }
#else
    (void)_d;
#endif
    /* JSON travels in WebSocket text frames, which must be valid UTF-8 */
    return mapepire_utf8_sanitize(_s, _len, _outlen);
}

int mapepire_dbc_utf8_to_char(mapepire_dbc *_d, const char *_s, size_t _len, mapepire_buf *_out)
{
#ifdef _WIN32
    if (_d->char_encoding == MAPEPIRE_CHARENC_ANSI && _len) {
        size_t units = mapepire_utf8_to_utf16(_s, _len, NULL, 0);
        WCHAR *w = (WCHAR *)malloc((units + 1) * sizeof(WCHAR));
        if (!w) return -1;
        mapepire_utf8_to_utf16(_s, _len, (uint16_t *)w, units + 1);
        int n = WideCharToMultiByte(CP_ACP, 0, w, (int)units, NULL, 0, NULL, NULL);
        if (mapepire_buf_reserve(_out, (size_t)n)) { free(w); return -1; }
        WideCharToMultiByte(CP_ACP, 0, w, (int)units, _out->data + _out->len, n, NULL, NULL);
        _out->len += (size_t)n;
        _out->data[_out->len] = '\0';
        free(w);
        return 0;
    }
#else
    (void)_d;
#endif
    return mapepire_buf_append(_out, _s, _len);
}

/* ---- JTOpen property string ------------------------------------------------------ */

/* Does the "k=v;k=v" property string already define key? */
static int props_has(const char *_props, const char *_key)
{
    if (!_props) return 0;
    size_t kl = strlen(_key);
    const char *p = _props;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!mapepire_strnicmp(p, _key, kl)) {
            const char *q = p + kl;
            while (*q == ' ') q++;
            if (*q == '=') return 1;
        }
        while (*p && *p != ';') p++;
    }
    return 0;
}

static void props_add(mapepire_buf *_b, const char *_user, const char *_key, const char *_value)
{
    if (props_has(_user, _key) || props_has(_b->data, _key)) return;
    if (_b->len && _b->data[_b->len - 1] != ';') mapepire_buf_append_char(_b, ';');
    mapepire_buf_printf(_b, "%s=%s", _key, _value);
}

static const char *isolation_prop(SQLUINTEGER _iso)
{
    switch (_iso) {
    case SQL_TXN_READ_UNCOMMITTED: return "read uncommitted";
    case SQL_TXN_READ_COMMITTED: return "read committed";
    case SQL_TXN_REPEATABLE_READ: return "repeatable read";
    case SQL_TXN_SERIALIZABLE: return "serializable";
    case 0x20: return "none"; /* IBM SQL_TXN_NOCOMMIT */
    default: return NULL;
    }
}

static const char *isolation_sql(SQLUINTEGER _iso)
{
    switch (_iso) {
    case SQL_TXN_READ_UNCOMMITTED: return "SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED";
    case SQL_TXN_READ_COMMITTED: return "SET TRANSACTION ISOLATION LEVEL READ COMMITTED";
    case SQL_TXN_REPEATABLE_READ: return "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ";
    case SQL_TXN_SERIALIZABLE: return "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE";
    case 0x20: return "SET TRANSACTION ISOLATION LEVEL NO COMMIT";
    default: return NULL;
    }
}

static char *build_props(mapepire_dbc *_d)
{
    const char *user = _d->user_props;
    mapepire_buf b;
    mapepire_buf_init(&b);
    if (user && *user) mapepire_buf_append_str(&b, user);
    /* ISO formats make date/time values unambiguous for conversion. */
    props_add(&b, user, "date format", "iso");
    props_add(&b, user, "time format", "iso");
    const char *naming = mapepire_conf_get(&_d->conf, "NAMING");
    if (naming && *naming) props_add(&b, user, "naming", naming);
    const char *libs = mapepire_conf_get(&_d->conf, "LIBRARIES");
    if (libs && *libs) props_add(&b, user, "libraries", libs);
    if (_d->autocommit == SQL_AUTOCOMMIT_OFF) props_add(&b, user, "auto commit", "false");
    const char *iso = isolation_prop(_d->txn_isolation);
    if (iso) props_add(&b, user, "transaction isolation", iso);
    if (_d->access_mode == SQL_MODE_READ_ONLY) props_add(&b, user, "access", "read only");
    return mapepire_buf_detach(&b);
}

/* ---- connect ------------------------------------------------------------------- */

static int conf_int(mapepire_dbc *_d, const char *_key, int _dflt)
{
    const char *v = mapepire_conf_get(&_d->conf, _key);
    if (!v || !*v) return _dflt;
    char *end;
    long n = strtol(v, &end, 10);
    if (*end) return _dflt;
    return (int)n;
}

static int conf_bool(mapepire_dbc *_d, const char *_key, int _dflt) { return mapepire_parse_bool(mapepire_conf_get(&_d->conf, _key), _dflt); }

static SQLRETURN do_connect(mapepire_dbc *_d)
{
    const char *dsn = mapepire_conf_get(&_d->conf, "DSN");
    if (dsn && *dsn) mapepire_conf_load_dsn(&_d->conf, dsn);

    const char *server = mapepire_conf_get(&_d->conf, "SERVER");
    const char *uid = mapepire_conf_get(&_d->conf, "UID");
    const char *pwd = mapepire_conf_get(&_d->conf, "PWD");
    if (!server || !*server) {
        mapepire_diag_add(&_d->h, "08001", 0, "No server specified: set SERVER (or HOST) in the connection string or DSN");
        return SQL_ERROR;
    }
    if (!uid || !*uid) {
        mapepire_diag_add(&_d->h, "28000", 0, "No user ID specified: set UID in the connection string or DSN");
        return SQL_ERROR;
    }

    /* optional AUTOCOMMIT / READONLY keywords set the initial attribute values */
    const char *ac = mapepire_conf_get(&_d->conf, "AUTOCOMMIT");
    if (ac && *ac) _d->autocommit = mapepire_parse_bool(ac, 1) ? SQL_AUTOCOMMIT_ON : SQL_AUTOCOMMIT_OFF;
    if (conf_bool(_d, "READONLY", 0)) _d->access_mode = SQL_MODE_READ_ONLY;

    _d->block_size = conf_int(_d, "BLOCKSIZE", MAPEPIRE_DEFAULT_BLOCK);
    if (_d->block_size <= 0) _d->block_size = MAPEPIRE_DEFAULT_BLOCK;

    const char *enc = mapepire_conf_get(&_d->conf, "CHARENCODING");
    if (enc && (!mapepire_stricmp(enc, "ansi") || !mapepire_stricmp(enc, "acp"))) _d->char_encoding = MAPEPIRE_CHARENC_ANSI;
    else if (enc && (!mapepire_stricmp(enc, "utf8") || !mapepire_stricmp(enc, "utf-8"))) _d->char_encoding = MAPEPIRE_CHARENC_UTF8;
    else _d->char_encoding = _d->ansi_app ? MAPEPIRE_CHARENC_ANSI : MAPEPIRE_CHARENC_UTF8;
#ifndef _WIN32
    _d->char_encoding = MAPEPIRE_CHARENC_UTF8;
#endif

    mapepire_connect_opts o;
    memset(&o, 0, sizeof o);
    o.ws.host = server;
    o.ws.port = conf_int(_d, "PORT", MAPEPIRE_DEFAULT_PORT);
    const char *path = mapepire_conf_get(&_d->conf, "PATH");
    o.ws.path = path && *path ? path : "/db/";
    o.ws.secure = conf_bool(_d, "SECURE", 1);
    o.ws.verify_cert = !conf_bool(_d, "TRUSTSERVERCERTIFICATE", 0);
    o.ws.cert_sha256 = mapepire_conf_get(&_d->conf, "CERTSHA256");
    o.ws.ca_file = mapepire_conf_get(&_d->conf, "CAFILE");
    o.ws.user = uid;
    o.ws.password = pwd ? pwd : "";
    int ct = _d->login_timeout ? (int)_d->login_timeout : conf_int(_d, "CONNECTTIMEOUT", 30);
    o.ws.connect_timeout = ct;
    o.ws.io_timeout = _d->connection_timeout ? (int)_d->connection_timeout : conf_int(_d, "SOCKETTIMEOUT", 0);
    const char *tr = mapepire_conf_get(&_d->conf, "TRANSPORT");
    if (tr && !mapepire_stricmp(tr, "socket")) o.ws.backend = MAPEPIRE_WS_BACKEND_SOCKET;
    else if (tr && !mapepire_stricmp(tr, "winhttp")) o.ws.backend = MAPEPIRE_WS_BACKEND_WINHTTP;
    else o.ws.backend = MAPEPIRE_WS_BACKEND_DEFAULT;
    o.technique = mapepire_conf_get(&_d->conf, "TECHNIQUE");
    o.application = mapepire_conf_get(&_d->conf, "APPLICATION");
    if (!o.application || !*o.application) o.application = "mapepire-odbc";

    free(_d->user_props);
    _d->user_props = mapepire_strdup(mapepire_conf_get(&_d->conf, "PROPS"));
    char *props = build_props(_d);
    o.props = props;

    mapepire_error e;
    memset(&e, 0, sizeof e);
    int rc = mapepire_client_open(&_d->cli, &o, &e);
    free(props);
    if (rc) {
        _d->cli = NULL;
        mapepire_diag_from_error(&_d->h, &e);
        return SQL_ERROR;
    }

    free(_d->server);
    _d->server = mapepire_strdup(server);
    free(_d->user);
    _d->user = mapepire_strdup(uid);
    for (char *p = _d->user; p && *p; p++) *p = (char)toupper((unsigned char)*p);
    free(_d->dsn);
    _d->dsn = mapepire_strdup(dsn ? dsn : "");
    free(_d->catalog);
    _d->catalog = NULL;
    _d->catalog_fetched = 0;

    if (!o.ws.secure)
        mapepire_diag_add(&_d->h, "01000", 0, "Connected without TLS (SECURE=no); credentials and data are not encrypted");
    else if (!o.ws.verify_cert && !(o.ws.cert_sha256 && *o.ws.cert_sha256))
        mapepire_diag_add(&_d->h, "01000", 0,
                    "TrustServerCertificate=yes: the server certificate was not verified. Prefer CertSHA256");
    return _d->h.diag.count ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLConnectW(SQLHDBC _connection_handle, SQLWCHAR *_server_name, SQLSMALLINT _name_length1,
                                        SQLWCHAR *_user_name, SQLSMALLINT _name_length2, SQLWCHAR *_authentication,
                                        SQLSMALLINT _name_length3)
{
    if (!MAPEPIRE_VALID(_connection_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
    mapepire_dbc *d = (mapepire_dbc *)_connection_handle;
    mapepire_diag_clear(&d->h);
    if (mapepire_dbc_connected(d)) {
        mapepire_diag_add(&d->h, "08002", 0, "Connection name in use");
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    mapepire_conf_free(&d->conf);
    char *dsn = mapepire_from_w(_server_name, _name_length1);
    char *uid = mapepire_from_w(_user_name, _name_length2);
    char *pwd = mapepire_from_w(_authentication, _name_length3);
    if (dsn) mapepire_conf_set(&d->conf, "DSN", dsn);
    if (uid && *uid) mapepire_conf_set(&d->conf, "UID", uid);
    if (pwd && *pwd) mapepire_conf_set(&d->conf, "PWD", pwd);
    free(dsn);
    free(uid);
    if (pwd) { memset(pwd, 0, strlen(pwd)); free(pwd); }
    return mapepire_ret(&d->h, do_connect(d));
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLDriverConnectW(SQLHDBC _connection_handle, SQLHWND _window_handle,
                                              SQLWCHAR *_in_connection_string, SQLSMALLINT _string_length1,
                                              SQLWCHAR *_out_connection_string, SQLSMALLINT _buffer_length,
                                              SQLSMALLINT *_string_length2_ptr, SQLUSMALLINT _driver_completion)
{
    (void)_window_handle;
    if (!MAPEPIRE_VALID(_connection_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
    mapepire_dbc *d = (mapepire_dbc *)_connection_handle;
    mapepire_diag_clear(&d->h);
    if (mapepire_dbc_connected(d)) {
        mapepire_diag_add(&d->h, "08002", 0, "Connection name in use");
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    if (_driver_completion != SQL_DRIVER_NOPROMPT && _driver_completion != SQL_DRIVER_COMPLETE &&
        _driver_completion != SQL_DRIVER_PROMPT && _driver_completion != SQL_DRIVER_COMPLETE_REQUIRED) {
        mapepire_diag_add(&d->h, "HY110", 0, "Invalid driver completion");
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    mapepire_conf_free(&d->conf);
    char *in = mapepire_from_w(_in_connection_string, _string_length1);
    char perr[256] = "";
    int prc = mapepire_conf_parse(&d->conf, in ? in : "", perr, sizeof perr);
    if (in) { memset(in, 0, strlen(in)); free(in); }
    if (prc) {
        mapepire_diag_add(&d->h, "08001", 0, "%s", perr);
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    /* This driver has no login dialog: prompting behaves like SQL_DRIVER_NOPROMPT. */
    SQLRETURN rc = do_connect(d);
    if (!SQL_SUCCEEDED(rc)) return mapepire_ret(&d->h, rc);

    char *outs = mapepire_conf_to_string(&d->conf);
    if (mapepire_to_w_chars(outs, _out_connection_string, _buffer_length, _string_length2_ptr)) {
        mapepire_diag_add(&d->h, "01004", 0, "String data, right truncated (output connection string)");
        rc = SQL_SUCCESS_WITH_INFO;
    }
    memset(outs, 0, strlen(outs));
    free(outs);
    return mapepire_ret(&d->h, rc);
}

static void free_statements(mapepire_dbc *_d)
{
    while (_d->stmts) mapepire_stmt_destroy(_d->stmts);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLDisconnect(SQLHDBC _connection_handle)
{
    if (!MAPEPIRE_VALID(_connection_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
    mapepire_dbc *d = (mapepire_dbc *)_connection_handle;
    mapepire_diag_clear(&d->h);
    if (!mapepire_dbc_connected(d)) {
        mapepire_diag_add(&d->h, "08003", 0, "Connection not open");
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    free_statements(d);
    mapepire_client_close(d->cli);
    d->cli = NULL;
    return SQL_SUCCESS;
}

void mapepire_dbc_destroy(mapepire_dbc *_d)
{
    free_statements(_d);
    if (_d->cli) mapepire_client_close(_d->cli);
    mapepire_conf_free(&_d->conf);
    mapepire_diag_free(&_d->h.diag);
    free(_d->server);
    free(_d->user);
    free(_d->dsn);
    free(_d->catalog);
    free(_d->user_props);
    mapepire_mutex_destroy(&_d->lock);
    _d->h.magic = 0;
    free(_d);
}

/* Run a statement that returns no rows, closing its server handle afterwards. */
static SQLRETURN run_simple(mapepire_dbc *_d, const char *_sql)
{
    mpjson *r = mapepire_req("sql");
    mpjson_set(r, "sql", mpjson_new_string(_sql));
    char id[32] = "";
    mapepire_error e;
    memset(&e, 0, sizeof e);
    mpjson *resp = mapepire_client_call(_d->cli, r, id, &e);
    if (id[0]) {
        mpjson *c = mapepire_req("sqlclose");
        mpjson_set(c, "cont_id", mpjson_new_string(id));
        mapepire_client_post(_d->cli, c);
    }
    if (!resp) return mapepire_diag_from_error(&_d->h, &e);
    mpjson_free(resp);
    return SQL_SUCCESS;
}

const char *mapepire_dbc_catalog(mapepire_dbc *_d)
{
    mapepire_mutex_lock(&_d->lock); /* several statements may ask at once */
    if (!_d->catalog_fetched && _d->cli) {
        _d->catalog_fetched = 1;
        mpjson *r = mapepire_req("sql");
        mpjson_set(r, "sql", mpjson_new_string("SELECT CURRENT SERVER FROM SYSIBM.SYSDUMMY1"));
        mpjson_set(r, "terse", mpjson_new_bool(1));
        char id[32] = "";
        mapepire_error e;
        mpjson *resp = mapepire_client_call(_d->cli, r, id, &e);
        if (id[0]) {
            mpjson *c = mapepire_req("sqlclose");
            mpjson_set(c, "cont_id", mpjson_new_string(id));
            mapepire_client_post(_d->cli, c);
        }
        if (resp) {
            mpjson *row = mpjson_at(mpjson_get(resp, "data"), 0);
            mpjson *cell = row && row->type == MPJSON_ARRAY ? mpjson_at(row, 0) : mpjson_at(row, 0);
            const char *v = mpjson_str(cell);
            if (v) {
                _d->catalog = mapepire_strdup(v);
                mapepire_trim(_d->catalog);
            }
            mpjson_free(resp);
        }
    }
    mapepire_mutex_unlock(&_d->lock);
    return _d->catalog ? _d->catalog : "";
}

/* ---- transactions ---------------------------------------------------------------- */

static SQLRETURN end_tran(mapepire_dbc *_d, SQLSMALLINT _op)
{
    if (!mapepire_dbc_connected(_d)) {
        mapepire_diag_add(&_d->h, "08003", 0, "Connection not open");
        return SQL_ERROR;
    }
    if (_op != SQL_COMMIT && _op != SQL_ROLLBACK) {
        mapepire_diag_add(&_d->h, "HY012", 0, "Invalid transaction operation code");
        return SQL_ERROR;
    }
    if (_d->autocommit == SQL_AUTOCOMMIT_ON) return SQL_SUCCESS;
    return run_simple(_d, _op == SQL_COMMIT ? "COMMIT" : "ROLLBACK");
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLEndTran(SQLSMALLINT _handle_type, SQLHANDLE _handle, SQLSMALLINT _completion_type)
{
    if (_handle_type == SQL_HANDLE_DBC) {
        if (!MAPEPIRE_VALID(_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
        mapepire_dbc *d = (mapepire_dbc *)_handle;
        mapepire_diag_clear(&d->h);
        return mapepire_ret(&d->h, end_tran(d, _completion_type));
    }
    if (_handle_type == SQL_HANDLE_ENV) {
        if (!MAPEPIRE_VALID(_handle, MAPEPIRE_H_ENV)) return SQL_INVALID_HANDLE;
        mapepire_env *e = (mapepire_env *)_handle;
        mapepire_diag_clear(&e->h);
        SQLRETURN rc = SQL_SUCCESS;
        mapepire_mutex_lock(&e->lock);
        for (mapepire_dbc *d = e->dbcs; d; d = d->next) {
            if (!mapepire_dbc_connected(d)) continue;
            mapepire_diag_clear(&d->h);
            if (!SQL_SUCCEEDED(end_tran(d, _completion_type))) rc = SQL_ERROR;
        }
        mapepire_mutex_unlock(&e->lock);
        if (rc == SQL_ERROR) mapepire_diag_add(&e->h, "25S01", 0, "Transaction state unknown: a connection failed to complete");
        return mapepire_ret(&e->h, rc);
    }
    return SQL_INVALID_HANDLE;
}

/* ---- attributes ------------------------------------------------------------------- */

static int has_open_results(mapepire_dbc *_d)
{
    int open = 0;
    mapepire_mutex_lock(&_d->lock);
    /* Only server cursors are lost by a reconnect; result sets the driver builds
     * itself (SQLGetTypeInfo, catalog enumerations) have no server cursor. */
    for (mapepire_stmt *s = _d->stmts; s && !open; s = s->next)
        if ((s->has_result && s->cursor[0]) || s->state == MAPEPIRE_ST_NEED_DATA) open = 1;
    mapepire_mutex_unlock(&_d->lock);
    return open;
}

/* Re-run "connect" on the existing socket with the current attribute values. */
static SQLRETURN reconnect(mapepire_dbc *_d)
{
    if (has_open_results(_d)) {
        mapepire_diag_add(&_d->h, "HY011", 0,
                    "This attribute can only be changed while no statement has an open cursor or pending data");
        return SQL_ERROR;
    }
    char *props = build_props(_d);
    mapepire_error e;
    memset(&e, 0, sizeof e);
    int rc = mapepire_client_reconnect(_d->cli, props, &e);
    free(props);
    if (rc) return mapepire_diag_from_error(&_d->h, &e);
    /* the server-side JDBC connection is new: cached state is gone */
    for (mapepire_stmt *s = _d->stmts; s; s = s->next) {
        s->described = 0;
        if (s->state == MAPEPIRE_ST_EXECUTED) s->state = s->sql ? MAPEPIRE_ST_PREPARED : MAPEPIRE_ST_ALLOC;
    }
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLSetConnectAttrW(SQLHDBC _connection_handle, SQLINTEGER _attribute, SQLPOINTER _value,
                                               SQLINTEGER _string_length)
{
    if (!MAPEPIRE_VALID(_connection_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
    mapepire_dbc *d = (mapepire_dbc *)_connection_handle;
    mapepire_diag_clear(&d->h);
    SQLUINTEGER v = (SQLUINTEGER)(SQLULEN)_value;
    SQLRETURN rc = SQL_SUCCESS;
    switch (_attribute) {
    case SQL_ATTR_AUTOCOMMIT: {
        if (v != SQL_AUTOCOMMIT_ON && v != SQL_AUTOCOMMIT_OFF) {
            mapepire_diag_add(&d->h, "HY024", 0, "Invalid attribute value");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        if (v == d->autocommit) break;
        if (mapepire_dbc_connected(d)) {
            if (has_open_results(d)) {
                mapepire_diag_add(&d->h, "HY011", 0,
                            "Autocommit can only be changed while no statement has an open cursor");
                return mapepire_ret(&d->h, SQL_ERROR);
            }
            /* ODBC: switching autocommit on commits the open transaction */
            if (v == SQL_AUTOCOMMIT_ON) {
                rc = run_simple(d, "COMMIT");
                if (!SQL_SUCCEEDED(rc)) return mapepire_ret(&d->h, rc);
            }
            SQLUINTEGER old = d->autocommit;
            d->autocommit = v;
            rc = reconnect(d);
            if (!SQL_SUCCEEDED(rc)) d->autocommit = old;
        } else {
            d->autocommit = v;
        }
        break;
    }
    case SQL_ATTR_TXN_ISOLATION: {
        if (!isolation_sql(v)) {
            mapepire_diag_add(&d->h, "HY024", 0, "Invalid attribute value");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        if (mapepire_dbc_connected(d)) {
            rc = run_simple(d, isolation_sql(v));
            if (!SQL_SUCCEEDED(rc)) return mapepire_ret(&d->h, rc);
        }
        d->txn_isolation = v;
        break;
    }
    case SQL_ATTR_ACCESS_MODE:
        if (v != SQL_MODE_READ_ONLY && v != SQL_MODE_READ_WRITE) {
            mapepire_diag_add(&d->h, "HY024", 0, "Invalid attribute value");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        if (mapepire_dbc_connected(d) && v != d->access_mode) {
            rc = run_simple(d, v == SQL_MODE_READ_ONLY ? "SET TRANSACTION READ ONLY" : "SET TRANSACTION READ WRITE");
            if (!SQL_SUCCEEDED(rc)) return mapepire_ret(&d->h, rc);
        }
        d->access_mode = v;
        break;
    case SQL_ATTR_LOGIN_TIMEOUT: d->login_timeout = v; break;
    case SQL_ATTR_CONNECTION_TIMEOUT: d->connection_timeout = v; break;
    case SQL_ATTR_METADATA_ID: d->metadata_id = v; break;
#ifdef SQL_ATTR_ANSI_APP
    case SQL_ATTR_ANSI_APP: d->ansi_app = v == SQL_AA_TRUE; break;
#endif
    case SQL_ATTR_CURRENT_CATALOG: {
        char *cat = _string_length == SQL_NTS || _string_length < 0
                        ? mapepire_from_w((SQLWCHAR *)_value, SQL_NTS)
                        : mapepire_from_w((SQLWCHAR *)_value, _string_length / (SQLINTEGER)sizeof(SQLWCHAR));
        int same = !mapepire_dbc_connected(d) || !mapepire_stricmp(cat ? cat : "", mapepire_dbc_catalog(d));
        free(cat);
        if (!same) {
            mapepire_diag_add(&d->h, "HYC00", 0, "Changing the current catalog is not supported");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        break;
    }
    case SQL_ATTR_ASYNC_ENABLE:
        if (v != SQL_ASYNC_ENABLE_OFF) {
            mapepire_diag_add(&d->h, "HYC00", 0, "Asynchronous execution is not supported");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        break;
    case SQL_ATTR_PACKET_SIZE:
    case SQL_ATTR_QUIET_MODE:
    case SQL_ATTR_TRACE:
    case SQL_ATTR_TRACEFILE:
    case SQL_ATTR_TRANSLATE_LIB:
    case SQL_ATTR_TRANSLATE_OPTION:
    case SQL_ATTR_ODBC_CURSORS:
        break;
#ifdef SQL_ATTR_RESET_CONNECTION
    case SQL_ATTR_RESET_CONNECTION: break;
#endif
    default:
        mapepire_diag_add(&d->h, "HY092", 0, "Invalid attribute/option identifier (%d)", (int)_attribute);
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    return mapepire_ret(&d->h, rc);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetConnectAttrW(SQLHDBC _connection_handle, SQLINTEGER _attribute, SQLPOINTER _value,
                                               SQLINTEGER _buffer_length, SQLINTEGER *_string_length)
{
    if (!MAPEPIRE_VALID(_connection_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
    mapepire_dbc *d = (mapepire_dbc *)_connection_handle;
    mapepire_diag_clear(&d->h);
    SQLUINTEGER v;
    switch (_attribute) {
    case SQL_ATTR_AUTOCOMMIT: v = d->autocommit; break;
    case SQL_ATTR_TXN_ISOLATION: v = d->txn_isolation ? d->txn_isolation : SQL_TXN_READ_UNCOMMITTED; break;
    case SQL_ATTR_ACCESS_MODE: v = d->access_mode; break;
    case SQL_ATTR_LOGIN_TIMEOUT: v = d->login_timeout; break;
    case SQL_ATTR_CONNECTION_TIMEOUT: v = d->connection_timeout; break;
    case SQL_ATTR_METADATA_ID: v = d->metadata_id; break;
    case SQL_ATTR_ASYNC_ENABLE: /* defined as SQLULEN */
        if (_value) *(SQLULEN *)_value = SQL_ASYNC_ENABLE_OFF;
        if (_string_length) *_string_length = sizeof(SQLULEN);
        return SQL_SUCCESS;
    case SQL_ATTR_PACKET_SIZE: v = 0; break;
    case SQL_ATTR_AUTO_IPD: v = SQL_FALSE; break;
    case SQL_ATTR_CONNECTION_DEAD:
        v = (!d->cli || mapepire_client_is_dead(d->cli)) ? SQL_CD_TRUE : SQL_CD_FALSE;
        break;
    case SQL_ATTR_CURRENT_CATALOG: {
        if (!mapepire_dbc_connected(d)) {
            mapepire_diag_add(&d->h, "08003", 0, "Connection not open");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        if (mapepire_to_w_bytes(mapepire_dbc_catalog(d), _value, _buffer_length, _string_length)) {
            mapepire_diag_add(&d->h, "01004", 0, "String data, right truncated");
            return mapepire_ret(&d->h, SQL_SUCCESS_WITH_INFO);
        }
        return SQL_SUCCESS;
    }
    default:
        mapepire_diag_add(&d->h, "HY092", 0, "Invalid attribute/option identifier (%d)", (int)_attribute);
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    if (_value) *(SQLUINTEGER *)_value = v;
    if (_string_length) *_string_length = sizeof(SQLUINTEGER);
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLNativeSqlW(SQLHDBC _connection_handle, SQLWCHAR *_in_statement_text, SQLINTEGER _text_length1,
                                          SQLWCHAR *_out_statement_text, SQLINTEGER _buffer_length,
                                          SQLINTEGER *_text_length2_ptr)
{
    if (!MAPEPIRE_VALID(_connection_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
    mapepire_dbc *d = (mapepire_dbc *)_connection_handle;
    mapepire_diag_clear(&d->h);
    if (!_in_statement_text) {
        mapepire_diag_add(&d->h, "HY009", 0, "Invalid use of null pointer");
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    /* Escape sequences are processed by the server's JDBC driver, so the text is unchanged. */
    char *sql = mapepire_from_w(_in_statement_text, _text_length1);
    SQLSMALLINT len = 0;
    int trunc = mapepire_to_w_chars(sql, _out_statement_text, (SQLSMALLINT)(_buffer_length > 32767 ? 32767 : _buffer_length), &len);
    if (_text_length2_ptr) *_text_length2_ptr = (SQLINTEGER)mapepire_utf8_to_utf16(sql, strlen(sql), NULL, 0);
    free(sql);
    if (trunc) {
        mapepire_diag_add(&d->h, "01004", 0, "String data, right truncated");
        return mapepire_ret(&d->h, SQL_SUCCESS_WITH_INFO);
    }
    return SQL_SUCCESS;
}

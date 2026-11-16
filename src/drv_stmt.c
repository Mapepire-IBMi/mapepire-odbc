/*
 * Statement handling: prepare/execute, parameters, fetching, metadata and
 * statement attributes.
 *
 * Execution model (see README "How it maps onto Mapepire"):
 *   - statements without parameters run with a "sql" request;
 *   - statements with parameters run with a fresh "prepare_sql_execute"
 *     every time (the real server cannot page the result of "execute");
 *   - SQLPrepare is deferred: "prepare_sql" is only sent when metadata is
 *     requested before execution, and that server handle is closed at once;
 *   - every server cursor is closed with a fire-and-forget "sqlclose".
 */
#include "drv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- helpers --------------------------------------------------------------------- */

void mapepire_cols_free(mapepire_col *_c, int _n)
{
    if (!_c) return;
    for (int i = 0; i < _n; i++) {
        free(_c[i].name);
        free(_c[i].label);
        free(_c[i].type_name);
        free(_c[i].table);
        free(_c[i].schema);
    }
    free(_c);
}

static void pdesc_free(mapepire_paramdesc *_p, int _n)
{
    if (!_p) return;
    for (int i = 0; i < _n; i++) free(_p[i].type_name);
    free(_p);
}

int mapepire_cols_from_meta(const mpjson *_meta, mapepire_col **_out, int *_n)
{
    *_out = NULL;
    *_n = 0;
    mpjson *cols = mpjson_get(_meta, "columns");
    size_t cnt = mpjson_len(cols);
    if (!cnt) return 0;
    mapepire_col *c = (mapepire_col *)calloc(cnt, sizeof *c);
    if (!c) return -1;
    for (size_t i = 0; i < cnt; i++) {
        mpjson *m = mpjson_at(cols, i);
        const char *name = mpjson_get_str(m, "name");
        const char *label = mpjson_get_str(m, "label");
        const char *type = mpjson_get_str(m, "type");
        if (!name || !*name) name = label ? label : "";
        c[i].name = mapepire_strdup(name);
        c[i].label = mapepire_strdup(label && *label ? label : name);
        c[i].type_name = mapepire_strdup(type ? type : "VARCHAR");
        c[i].table = mapepire_strdup(mpjson_get_str(m, "table") ? mpjson_get_str(m, "table") : "");
        c[i].schema = mapepire_strdup("");
        mapepire_type_describe(c[i].type_name, (long)mpjson_get_int(m, "precision", 0), (long)mpjson_get_int(m, "scale", 0), &c[i]);
        mpjson *nl = mpjson_get(m, "nullable");
        c[i].nullable = mpjson_is_null(nl) ? SQL_NULLABLE_UNKNOWN : (SQLSMALLINT)mpjson_get_int(m, "nullable", 2);
        if (c[i].nullable < 0 || c[i].nullable > 2) c[i].nullable = SQL_NULLABLE_UNKNOWN;
        c[i].autoinc = mpjson_get_bool(m, "autoIncrement", 0);
        mpjson *ro = mpjson_get(m, "readOnly");
        mpjson *wr = mpjson_get(m, "writeable");
        if (!mpjson_is_null(ro) && mpjson_get_bool(m, "readOnly", 0)) c[i].updatable = SQL_ATTR_READONLY;
        else if (!mpjson_is_null(wr) && mpjson_get_bool(m, "writeable", 0)) c[i].updatable = SQL_ATTR_WRITE;
        else c[i].updatable = SQL_ATTR_READWRITE_UNKNOWN;
    }
    *_out = c;
    *_n = (int)cnt;
    return 0;
}

int mapepire_count_markers(const char *_sql)
{
    int n = 0;
    for (const char *p = _sql; p && *p; p++) {
        if (*p == '\'' || *p == '"') {
            char q = *p++;
            while (*p) {
                if (*p == q) {
                    if (p[1] == q) { p += 2; continue; }
                    break;
                }
                p++;
            }
            if (!*p) break;
        } else if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
            if (!*p) break;
        } else if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (!*p) break;
            p++;
        } else if (*p == '?') {
            n++;
        }
    }
    return n;
}

static void post_close(mapepire_stmt *_s, const char *_id)
{
    if (!_id || !*_id || !_s->dbc->cli || mapepire_client_is_dead(_s->dbc->cli)) return;
    mpjson *c = mapepire_req("sqlclose");
    mpjson_set(c, "cont_id", mpjson_new_string(_id));
    mapepire_client_post(_s->dbc->cli, c);
}

static void gd_reset(mapepire_stmt *_s)
{
    _s->gd_col = 0;
    _s->gd_started = 0;
    _s->gd_finished = 0;
    _s->gd_off = 0;
    mapepire_buf_clear(&_s->gd_cache);
}

void mapepire_stmt_close_cursor(mapepire_stmt *_s)
{
    if (_s->cursor[0]) post_close(_s, _s->cursor);
    _s->cursor[0] = '\0';
    mpjson_free(_s->resp);
    _s->resp = NULL;
    mpjson_free(_s->prev_resp);
    _s->prev_resp = NULL;
    _s->block = NULL;
    _s->cur_row = NULL;
    _s->pos = 0;
    _s->is_done = 1;
    _s->has_result = 0;
    _s->fixup = MAPEPIRE_FIX_NONE;
    gd_reset(_s);
}

void mapepire_stmt_reset_result(mapepire_stmt *_s)
{
    mapepire_stmt_close_cursor(_s);
    mapepire_cols_free(_s->cols, _s->ncols);
    _s->cols = NULL;
    _s->ncols = 0;
    _s->row_count = -1;
    _s->rows_returned = 0;
    _s->cur_row_number = 0;
    _s->more_results_done = 0;
}

static void invalidate_described(mapepire_stmt *_s)
{
    mapepire_cols_free(_s->pcols, _s->npcols);
    _s->pcols = NULL;
    _s->npcols = 0;
    pdesc_free(_s->pdesc, _s->npdesc);
    _s->pdesc = NULL;
    _s->npdesc = 0;
    _s->described = 0;
}

static void free_pbind(mapepire_stmt *_s)
{
    for (int i = 0; i < _s->npbind; i++) mapepire_buf_free(&_s->pbind[i].dae_data);
    free(_s->pbind);
    _s->pbind = NULL;
    _s->npbind = 0;
}

static void init_desc(mapepire_desc *_d, mapepire_stmt *_s, int _kind)
{
    _d->h.magic = MAPEPIRE_H_DESC;
    _d->stmt = _s;
    _d->kind = _kind;
}

mapepire_stmt *mapepire_stmt_new(mapepire_dbc *_d)
{
    mapepire_stmt *s = (mapepire_stmt *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->h.magic = MAPEPIRE_H_STMT;
    s->dbc = _d;
    init_desc(&s->ard, s, SQL_ATTR_APP_ROW_DESC);
    init_desc(&s->apd, s, SQL_ATTR_APP_PARAM_DESC);
    init_desc(&s->ird, s, SQL_ATTR_IMP_ROW_DESC);
    init_desc(&s->ipd, s, SQL_ATTR_IMP_PARAM_DESC);
    mapepire_buf_init(&s->gd_cache);
    s->row_count = -1;
    s->is_done = 1;
    s->row_array_size = 1;
    s->row_bind_type = SQL_BIND_BY_COLUMN;
    s->paramset_size = 1;
    s->param_bind_type = SQL_PARAM_BIND_BY_COLUMN;
    s->retrieve_data = SQL_RD_ON;
    s->noscan = SQL_NOSCAN_OFF;
    s->cursor_type = SQL_CURSOR_FORWARD_ONLY;
    s->concurrency = SQL_CONCUR_READ_ONLY;
    s->use_bookmarks = SQL_UB_OFF;
    s->metadata_id = _d->metadata_id;
    s->dae_current = -1;
    static volatile long counter = 0;
#ifdef _WIN32
    long n = InterlockedIncrement(&counter);
#else
    long n = __atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST);
#endif
    mapepire_snprintf(s->cursor_name, sizeof s->cursor_name, "SQL_CUR%ld", n);
    mapepire_mutex_lock(&_d->lock);
    s->next = _d->stmts;
    _d->stmts = s;
    mapepire_mutex_unlock(&_d->lock);
    return s;
}

void mapepire_stmt_destroy(mapepire_stmt *_s)
{
    mapepire_dbc *d = _s->dbc;
    mapepire_mutex_lock(&d->lock);
    for (mapepire_stmt **pp = &d->stmts; *pp; pp = &(*pp)->next) {
        if (*pp == _s) { *pp = _s->next; break; }
    }
    mapepire_mutex_unlock(&d->lock);
    mapepire_stmt_reset_result(_s);
    invalidate_described(_s);
    free_pbind(_s);
    free(_s->cbind);
    free(_s->sql);
    mapepire_buf_free(&_s->gd_cache);
    mapepire_diag_free(&_s->h.diag);
    mapepire_diag_free(&_s->ard.h.diag);
    mapepire_diag_free(&_s->apd.h.diag);
    mapepire_diag_free(&_s->ird.h.diag);
    mapepire_diag_free(&_s->ipd.h.diag);
    _s->h.magic = _s->ard.h.magic = _s->apd.h.magic = _s->ird.h.magic = _s->ipd.h.magic = 0;
    free(_s);
}

static SQLRETURN need_conn(mapepire_stmt *_s)
{
    if (!_s->dbc->cli) {
        mapepire_diag_add(&_s->h, "08003", 0, "Connection not open");
        return SQL_ERROR;
    }
    if (mapepire_client_is_dead(_s->dbc->cli)) {
        mapepire_diag_add(&_s->h, "08S01", 0, "Communication link failure: the connection to the server was lost");
        return SQL_ERROR;
    }
    return SQL_SUCCESS;
}

/* SQL_ATTR_QUERY_TIMEOUT bounds the wait for each reply (MAPEPIRE_CALL_DEFAULT_TIMEOUT otherwise). */
static long query_timeout_ms(mapepire_stmt *_s)
{
    return _s->query_timeout ? (long)_s->query_timeout * 1000L : MAPEPIRE_CALL_DEFAULT_TIMEOUT;
}

static int block_rows(mapepire_stmt *_s)
{
    int n = _s->dbc->block_size;
    if (_s->max_rows && (SQLULEN)n > _s->max_rows) n = (int)_s->max_rows;
    SQLULEN ras = _s->row_array_size;
    if (ras > (SQLULEN)n && ras < 100000) n = (int)ras;
    return n > 0 ? n : MAPEPIRE_DEFAULT_BLOCK;
}

/* ---- describe (prepare_sql) ------------------------------------------------------- */

static SQLRETURN describe(mapepire_stmt *_s)
{
    if (_s->described) return SQL_SUCCESS;
    if (!_s->sql) {
        mapepire_diag_add(&_s->h, "HY010", 0, "Function sequence error: no statement has been prepared");
        return SQL_ERROR;
    }
    SQLRETURN rc = need_conn(_s);
    if (!SQL_SUCCEEDED(rc)) return rc;
    mpjson *r = mapepire_req("prepare_sql");
    mpjson_set(r, "sql", mpjson_new_string(_s->sql));
    mpjson_set(r, "terse", mpjson_new_bool(1));
    char id[32] = "";
    mapepire_error e;
    memset(&e, 0, sizeof e);
    mpjson *resp = mapepire_client_call(_s->dbc->cli, r, id, &e);
    post_close(_s, id);
    if (!resp) return mapepire_diag_from_error(&_s->h, &e);
    mpjson *meta = mpjson_get(resp, "metadata");
    invalidate_described(_s);
    mapepire_cols_from_meta(meta, &_s->pcols, &_s->npcols);
    mpjson *params = mpjson_get(meta, "parameters");
    size_t np = mpjson_len(params);
    long long pc = mpjson_get_int(resp, "parameter_count", (long long)np);
    if (pc < (long long)np) pc = (long long)np;
    if (pc > 0) {
        _s->pdesc = (mapepire_paramdesc *)calloc((size_t)pc, sizeof *_s->pdesc);
        if (_s->pdesc) {
            _s->npdesc = (int)pc;
            for (long long i = 0; i < pc; i++) {
                mpjson *p = mpjson_at(params, (size_t)i);
                const char *t = mpjson_get_str(p, "type");
                mapepire_col tmp;
                memset(&tmp, 0, sizeof tmp);
                mapepire_type_describe(t ? t : "VARCHAR", (long)mpjson_get_int(p, "precision", 0), (long)mpjson_get_int(p, "scale", 0), &tmp);
                _s->pdesc[i].type_name = mapepire_strdup(t ? t : "VARCHAR");
                _s->pdesc[i].sql_type = tmp.sql_type;
                _s->pdesc[i].size = tmp.size;
                _s->pdesc[i].digits = tmp.digits;
                _s->pdesc[i].nullable = SQL_NULLABLE_UNKNOWN;
                const char *mode = mpjson_get_str(p, "mode");
                _s->pdesc[i].mode = !mode ? SQL_PARAM_INPUT
                                   : !mapepire_stricmp(mode, "OUT") ? SQL_PARAM_OUTPUT
                                   : !mapepire_stricmp(mode, "INOUT") ? SQL_PARAM_INPUT_OUTPUT
                                   : !mapepire_stricmp(mode, "IN") ? SQL_PARAM_INPUT : SQL_PARAM_TYPE_UNKNOWN;
            }
        }
    }
    mpjson_free(resp);
    _s->described = 1;
    return SQL_SUCCESS;
}

/* ---- result handling ---------------------------------------------------------------- */

static void install_result(mapepire_stmt *_s, mpjson *_resp, const char *_id)
{
    _s->resp = _resp;
    _s->block = mpjson_get(_resp, "data");
    _s->pos = 0;
    _s->is_done = mpjson_get_bool(_resp, "is_done", 1);
    _s->has_result = 1;
    mapepire_snprintf(_s->cursor, sizeof _s->cursor, "%s", _id ? _id : "");
    _s->row_count = -1;
}

void mapepire_stmt_local_result(mapepire_stmt *_s, const char *const *_names, const SQLSMALLINT *_types, int _ncols, mpjson *_rows)
{
    mapepire_stmt_reset_result(_s);
    _s->cols = (mapepire_col *)calloc((size_t)_ncols, sizeof *_s->cols);
    _s->ncols = _ncols;
    for (int i = 0; i < _ncols; i++) {
        mapepire_col *c = &_s->cols[i];
        c->name = mapepire_strdup(_names[i]);
        c->label = mapepire_strdup(_names[i]);
        const char *tn = _types[i] == SQL_SMALLINT ? "SMALLINT" : _types[i] == SQL_INTEGER ? "INTEGER" : "VARCHAR";
        c->type_name = mapepire_strdup(tn);
        c->table = mapepire_strdup("");
        c->schema = mapepire_strdup("");
        mapepire_type_describe(tn, _types[i] == SQL_VARCHAR ? 128 : 0, 0, c);
        c->nullable = SQL_NULLABLE;
    }
    mpjson *resp = mpjson_new(MPJSON_OBJECT);
    mpjson_set(resp, "data", _rows ? _rows : mpjson_new(MPJSON_ARRAY));
    mpjson_set(resp, "is_done", mpjson_new_bool(1));
    install_result(_s, resp, NULL);
    _s->state = MAPEPIRE_ST_EXECUTED;
}

static void apply_fixup(mapepire_stmt *_s, mpjson *_row)
{
    if (_s->fixup == MAPEPIRE_FIX_NONE || !_row || _row->type != MPJSON_ARRAY) return;
    const char *tn = mpjson_str(mpjson_at(_row, (size_t)_s->fixup_type_col));
    if (!tn) return;
    mapepire_col c;
    memset(&c, 0, sizeof c);
    if (!mapepire_type_describe(tn, 0, 0, &c)) return; /* unknown type name: keep the server's codes */
    mpjson *dt = mpjson_new_int(mapepire_odbc2_type(_s->dbc, c.sql_type));
    if (dt && (size_t)_s->fixup_data_col < _row->count) {
        mpjson_free(_row->items[_s->fixup_data_col]);
        _row->items[_s->fixup_data_col] = dt;
    } else {
        mpjson_free(dt);
    }
    if (_s->fixup_sqltype_col >= 0 && (size_t)_s->fixup_sqltype_col < _row->count) {
        mpjson_free(_row->items[_s->fixup_sqltype_col]);
        _row->items[_s->fixup_sqltype_col] = mpjson_new_int(mapepire_type_verbose(c.sql_type));
    }
    if (_s->fixup_sub_col >= 0 && (size_t)_s->fixup_sub_col < _row->count) {
        SQLSMALLINT sub = mapepire_type_datetime_sub(c.sql_type);
        mpjson_free(_row->items[_s->fixup_sub_col]);
        _row->items[_s->fixup_sub_col] = sub ? mpjson_new_int(sub) : mpjson_new(MPJSON_NULL);
    }
}

/* Advance to the next row; fetches the next block when needed. */
static SQLRETURN next_row(mapepire_stmt *_s, mpjson **_row)
{
    *_row = NULL;
    if (_s->max_rows && (SQLULEN)_s->rows_returned >= _s->max_rows) return SQL_NO_DATA;
    while (_s->pos >= mpjson_len(_s->block)) {
        if (_s->is_done || !_s->cursor[0]) return SQL_NO_DATA;
        SQLRETURN rc = need_conn(_s);
        if (!SQL_SUCCEEDED(rc)) return rc;
        mpjson *r = mapepire_req("sqlmore");
        mpjson_set(r, "cont_id", mpjson_new_string(_s->cursor));
        mpjson_set(r, "rows", mpjson_new_int(block_rows(_s)));
        mapepire_error e;
        memset(&e, 0, sizeof e);
        mpjson *resp = mapepire_client_call_timeout(_s->dbc->cli, r, NULL, query_timeout_ms(_s), &e);
        if (!resp) return mapepire_diag_from_error(&_s->h, &e);
        /* rows of the previous block may still be referenced by this rowset
         * (cur_row, bound conversions): retire it rather than freeing it */
        mpjson_free(_s->prev_resp);
        _s->prev_resp = _s->resp;
        _s->resp = resp;
        _s->block = mpjson_get(resp, "data");
        _s->pos = 0;
        _s->is_done = mpjson_get_bool(resp, "is_done", 1);
        if (!mpjson_len(_s->block) && !_s->is_done) {
            /* defensive: an empty, unfinished block would loop forever */
            _s->is_done = 1;
        }
    }
    *_row = mpjson_at(_s->block, _s->pos++);
    apply_fixup(_s, *_row);
    _s->rows_returned++;
    _s->cur_row_number++;
    return SQL_SUCCESS;
}

static const mpjson *row_cell(mapepire_stmt *_s, const mpjson *_row, int _idx)
{
    if (!_row) return NULL;
    if (_row->type == MPJSON_ARRAY) return mpjson_at(_row, (size_t)_idx);
    if (_row->type == MPJSON_OBJECT) {
        const mpjson *v = mpjson_get(_row, _s->cols[_idx].name);
        if (!v && _s->cols[_idx].label) v = mpjson_get(_row, _s->cols[_idx].label);
        return v;
    }
    return NULL;
}

/* ---- parameter handling ------------------------------------------------------------- */

static int is_dae(SQLLEN _ind) { return _ind == SQL_DATA_AT_EXEC || _ind <= SQL_LEN_DATA_AT_EXEC_OFFSET; }

static SQLLEN *param_ind_ptr(mapepire_stmt *_s, mapepire_parambind *_pb, SQLULEN _row)
{
    if (!_pb->ind) return NULL;
    SQLLEN offset = _s->param_bind_offset ? (SQLLEN)*_s->param_bind_offset : 0;
    if (_s->param_bind_type == SQL_PARAM_BIND_BY_COLUMN) return (SQLLEN *)((char *)_pb->ind + offset) + _row;
    return (SQLLEN *)((char *)_pb->ind + offset + (SQLLEN)_row * (SQLLEN)_s->param_bind_type);
}

static int nparams_to_send(mapepire_stmt *_s)
{
    if (_s->described && _s->npdesc) return _s->npdesc;
    return _s->nmarkers;
}

/* Build the JSON "parameters" value; *out NULL when there are no parameters. */
static SQLRETURN build_params(mapepire_stmt *_s, int _n, mpjson **_out, int *_rows_sent)
{
    *_out = NULL;
    *_rows_sent = 0;
    if (_n <= 0) return SQL_SUCCESS;
    if (_s->npbind < _n) {
        mapepire_diag_add(&_s->h, "07002", 0, "COUNT field incorrect: the statement has %d parameter markers but only %d are bound",
                    _n, _s->npbind);
        return SQL_ERROR;
    }
    for (int i = 0; i < _n; i++) {
        if (!_s->pbind[i].bound) {
            mapepire_diag_add(&_s->h, "07002", 0, "COUNT field incorrect: parameter %d is not bound", i + 1);
            return SQL_ERROR;
        }
    }
    SQLULEN sets = _s->paramset_size ? _s->paramset_size : 1;
    mpjson *all = sets > 1 ? mpjson_new(MPJSON_ARRAY) : NULL;
    for (SQLULEN r = 0; r < sets; r++) {
        if (_s->param_status) _s->param_status[r] = SQL_PARAM_UNUSED;
        if (_s->param_operation && _s->param_operation[r] == SQL_PARAM_IGNORE) continue;
        mpjson *row = mpjson_new(MPJSON_ARRAY);
        for (int i = 0; i < _n; i++) {
            mpjson *v = NULL;
            SQLRETURN rc = mapepire_param_to_json(_s, i, r, &v);
            if (!SQL_SUCCEEDED(rc)) {
                if (_s->param_status) _s->param_status[r] = SQL_PARAM_ERROR;
                mpjson_free(row);
                mpjson_free(all);
                return rc;
            }
            mpjson_push(row, v);
        }
        if (_s->param_status) _s->param_status[r] = SQL_PARAM_SUCCESS;
        (*_rows_sent)++;
        if (all) mpjson_push(all, row);
        else *_out = row;
    }
    if (all) *_out = all;
    return SQL_SUCCESS;
}

/* Copy output parameter values from a response into the bound buffers. */
static SQLRETURN store_output_params(mapepire_stmt *_s, const mpjson *_resp)
{
    mpjson *outs = mpjson_get(_resp, "output_parms");
    SQLRETURN rc = SQL_SUCCESS;
    for (size_t k = 0; k < mpjson_len(outs); k++) {
        mpjson *o = mpjson_at(outs, k);
        long long idx = mpjson_get_int(o, "index", 0);
        if (idx < 1 || idx > _s->npbind) continue;
        mapepire_parambind *pb = &_s->pbind[idx - 1];
        if (pb->io != SQL_PARAM_OUTPUT && pb->io != SQL_PARAM_INPUT_OUTPUT) continue;
        if (!pb->ptr && !pb->ind) continue;
        mapepire_col c;
        memset(&c, 0, sizeof c);
        const char *t = mpjson_get_str(o, "type");
        mapepire_type_describe(t ? t : "VARCHAR", (long)mpjson_get_int(o, "precision", 0), (long)mpjson_get_int(o, "scale", 0), &c);
        SQLSMALLINT ct = pb->ctype == SQL_C_DEFAULT ? mapepire_default_ctype(pb->sqltype) : pb->ctype;
        SQLLEN *ind = param_ind_ptr(_s, pb, 0);
        char *target = (char *)pb->ptr;
        if (target && _s->param_bind_offset) target += *_s->param_bind_offset;
        SQLRETURN r = mapepire_cell_to_c(_s, &c, mpjson_get(o, "value"), ct, target, pb->buflen, ind, SQL_NO_ROW_NUMBER,
                                   (SQLINTEGER)idx, NULL);
        if (r == SQL_ERROR) rc = SQL_ERROR;
        else if (r == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) rc = SQL_SUCCESS_WITH_INFO;
    }
    return rc;
}

/* Execute sql with optional parameters (ownership taken). */
static SQLRETURN execute_sql(mapepire_stmt *_s, const char *_sql, mpjson *_params, int _batch_rows)
{
    mapepire_stmt_reset_result(_s);
    SQLRETURN rc = need_conn(_s);
    if (!SQL_SUCCEEDED(rc)) { mpjson_free(_params); return rc; }
    mpjson *r = mapepire_req(_params ? "prepare_sql_execute" : "sql");
    mpjson_set(r, "sql", mpjson_new_string(_sql));
    if (_params) mpjson_set(r, "parameters", _params);
    mpjson_set(r, "rows", mpjson_new_int(block_rows(_s)));
    mpjson_set(r, "terse", mpjson_new_bool(1));
    char id[32] = "";
    mapepire_error e;
    memset(&e, 0, sizeof e);
    mpjson *resp = mapepire_client_call_timeout(_s->dbc->cli, r, id, query_timeout_ms(_s), &e);
    if (_s->params_processed) *_s->params_processed = (SQLULEN)(_batch_rows > 0 ? _batch_rows : 1);
    if (!resp) {
        post_close(_s, id); /* the server keeps failed statements registered */
        if (_s->param_status && _batch_rows > 0)
            for (SQLULEN i = 0; i < _s->paramset_size; i++)
                if (_s->param_status[i] == SQL_PARAM_SUCCESS) _s->param_status[i] = SQL_PARAM_ERROR;
        _s->state = _s->sql ? MAPEPIRE_ST_PREPARED : MAPEPIRE_ST_ALLOC;
        return mapepire_diag_from_error(&_s->h, &e);
    }
    _s->state = MAPEPIRE_ST_EXECUTED;
    rc = SQL_SUCCESS;
    if (_params) rc = store_output_params(_s, resp);
    if (mpjson_get_bool(resp, "has_results", 0)) {
        mapepire_cols_from_meta(mpjson_get(resp, "metadata"), &_s->cols, &_s->ncols);
        install_result(_s, resp, id);
    } else {
        _s->row_count = (SQLLEN)mpjson_get_int(resp, "update_count", -1);
        mpjson_free(resp);
        post_close(_s, id);
    }
    return rc;
}

SQLRETURN mapepire_stmt_run_query(mapepire_stmt *_s, const char *_sql, mpjson *_params)
{
    if (_params && !mpjson_len(_params)) {
        mpjson_free(_params);
        _params = NULL;
    }
    SQLRETURN rc = execute_sql(_s, _sql, _params, 0);
    return rc;
}

/* Run the prepared statement with the current bindings. */
static SQLRETURN exec_bound(mapepire_stmt *_s)
{
    int n = nparams_to_send(_s);
    mpjson *params = NULL;
    int rows_sent = 0;
    SQLRETURN rc = build_params(_s, n, &params, &rows_sent);
    if (!SQL_SUCCEEDED(rc)) return rc;
    if (n > 0 && rows_sent == 0) {
        /* every parameter set was SQL_PARAM_IGNORE */
        if (_s->params_processed) *_s->params_processed = 0;
        _s->state = MAPEPIRE_ST_EXECUTED;
        _s->row_count = 0;
        return SQL_SUCCESS;
    }
    return execute_sql(_s, _s->sql, params, (_s->paramset_size > 1) ? rows_sent : 0);
}

static SQLRETURN start_execute(mapepire_stmt *_s)
{
    if (_s->state == MAPEPIRE_ST_NEED_DATA) {
        mapepire_diag_add(&_s->h, "HY010", 0, "Function sequence error: data-at-execution parameters are pending");
        return SQL_ERROR;
    }
    if (_s->has_result) {
        mapepire_diag_add(&_s->h, "24000", 0, "Invalid cursor state: close the open cursor before executing again");
        return SQL_ERROR;
    }
    int n = nparams_to_send(_s);
    /* data-at-execution parameters? */
    int any = 0;
    for (int i = 0; i < n && i < _s->npbind; i++) {
        mapepire_parambind *pb = &_s->pbind[i];
        pb->dae = 0;
        pb->dae_done = 0;
        pb->dae_null = 0;
        mapepire_buf_clear(&pb->dae_data);
        SQLLEN *ind = param_ind_ptr(_s, pb, 0);
        if (pb->bound && pb->io != SQL_PARAM_OUTPUT && ind && is_dae(*ind)) {
            pb->dae = 1;
            any = 1;
        }
    }
    if (any) {
        if (_s->paramset_size > 1) {
            mapepire_diag_add(&_s->h, "HYC00", 0, "Data-at-execution parameters cannot be combined with parameter arrays");
            return SQL_ERROR;
        }
        _s->state = MAPEPIRE_ST_NEED_DATA;
        _s->dae_current = -1;
        return SQL_NEED_DATA;
    }
    return exec_bound(_s);
}

static void set_sql(mapepire_stmt *_s, char *_sql)
{
    free(_s->sql);
    _s->sql = _sql;
    _s->nmarkers = mapepire_count_markers(_sql);
    invalidate_described(_s);
}

/* ---- API: prepare / execute ------------------------------------------------------- */

#define STMT_ENTRY(_hstmt)                                    \
    if (!MAPEPIRE_VALID(_hstmt, MAPEPIRE_H_STMT)) return SQL_INVALID_HANDLE; \
    mapepire_stmt *s = (mapepire_stmt *)(_hstmt);                          \
    mapepire_diag_clear(&s->h)

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLPrepareW(SQLHSTMT _statement_handle, SQLWCHAR *_statement_text, SQLINTEGER _text_length)
{
    STMT_ENTRY(_statement_handle);
    if (!_statement_text) {
        mapepire_diag_add(&s->h, "HY009", 0, "Invalid use of null pointer");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (s->has_result) {
        mapepire_diag_add(&s->h, "24000", 0, "Invalid cursor state");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    mapepire_stmt_reset_result(s);
    set_sql(s, mapepire_from_w(_statement_text, _text_length));
    s->state = MAPEPIRE_ST_PREPARED;
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLExecute(SQLHSTMT _statement_handle)
{
    STMT_ENTRY(_statement_handle);
    if (!s->sql || s->state == MAPEPIRE_ST_ALLOC) {
        mapepire_diag_add(&s->h, "HY010", 0, "Function sequence error: no statement has been prepared");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    return mapepire_ret(&s->h, start_execute(s));
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLExecDirectW(SQLHSTMT _statement_handle, SQLWCHAR *_statement_text, SQLINTEGER _text_length)
{
    STMT_ENTRY(_statement_handle);
    if (!_statement_text) {
        mapepire_diag_add(&s->h, "HY009", 0, "Invalid use of null pointer");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (s->has_result) {
        mapepire_diag_add(&s->h, "24000", 0, "Invalid cursor state");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    mapepire_stmt_reset_result(s);
    set_sql(s, mapepire_from_w(_statement_text, _text_length));
    s->state = MAPEPIRE_ST_PREPARED;
    return mapepire_ret(&s->h, start_execute(s));
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLParamData(SQLHSTMT _statement_handle, SQLPOINTER *_value)
{
    STMT_ENTRY(_statement_handle);
    if (s->state != MAPEPIRE_ST_NEED_DATA) {
        mapepire_diag_add(&s->h, "HY010", 0, "Function sequence error");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (s->dae_current >= 0) s->pbind[s->dae_current].dae_done = 1;
    int n = nparams_to_send(s);
    for (int i = s->dae_current + 1; i < n && i < s->npbind; i++) {
        if (s->pbind[i].dae && !s->pbind[i].dae_done) {
            s->dae_current = i;
            if (_value) *_value = s->pbind[i].ptr;
            return mapepire_ret(&s->h, SQL_NEED_DATA);
        }
    }
    s->dae_current = -1;
    s->state = MAPEPIRE_ST_PREPARED;
    SQLRETURN rc = exec_bound(s);
    for (int i = 0; i < s->npbind; i++) {
        s->pbind[i].dae = 0;
        mapepire_buf_clear(&s->pbind[i].dae_data);
    }
    return mapepire_ret(&s->h, rc);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLPutData(SQLHSTMT _statement_handle, SQLPOINTER _data, SQLLEN _str_len_or_ind)
{
    STMT_ENTRY(_statement_handle);
    if (s->state != MAPEPIRE_ST_NEED_DATA || s->dae_current < 0) {
        mapepire_diag_add(&s->h, "HY010", 0, "Function sequence error");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    mapepire_parambind *pb = &s->pbind[s->dae_current];
    if (_str_len_or_ind == SQL_NULL_DATA) {
        pb->dae_null = 1;
        return SQL_SUCCESS;
    }
    if (!_data && _str_len_or_ind != 0) {
        mapepire_diag_add(&s->h, "HY009", 0, "Invalid use of null pointer");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    SQLSMALLINT ct = pb->ctype == SQL_C_DEFAULT ? mapepire_default_ctype(pb->sqltype) : pb->ctype;
    size_t n;
    if (_str_len_or_ind == SQL_NTS) {
        if (ct == SQL_C_WCHAR) {
            const SQLWCHAR *w = (const SQLWCHAR *)_data;
            size_t u = 0;
            while (w[u]) u++;
            n = u * sizeof(SQLWCHAR);
        } else {
            n = strlen((const char *)_data);
        }
    } else if (_str_len_or_ind < 0) {
        mapepire_diag_add(&s->h, "HY090", 0, "Invalid string or buffer length");
        return mapepire_ret(&s->h, SQL_ERROR);
    } else {
        n = (size_t)_str_len_or_ind;
    }
    SQLLEN fixed = mapepire_ctype_size(ct);
    if (fixed) n = (size_t)fixed;
    if (fixed && pb->dae_data.len) {
        mapepire_diag_add(&s->h, "22001", 0, "Fixed-length data may only be sent once");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (n && mapepire_buf_append(&pb->dae_data, _data, n)) {
        mapepire_diag_add(&s->h, "HY001", 0, "Out of memory");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLNumParams(SQLHSTMT _statement_handle, SQLSMALLINT *_parameter_count)
{
    STMT_ENTRY(_statement_handle);
    if (!s->sql) {
        mapepire_diag_add(&s->h, "HY010", 0, "Function sequence error");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_parameter_count) *_parameter_count = (SQLSMALLINT)nparams_to_send(s);
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLBindParameter(SQLHSTMT _statement_handle, SQLUSMALLINT _parameter_number,
                                             SQLSMALLINT _input_output_type, SQLSMALLINT _value_type,
                                             SQLSMALLINT _parameter_type, SQLULEN _column_size,
                                             SQLSMALLINT _decimal_digits, SQLPOINTER _parameter_value_ptr,
                                             SQLLEN _buffer_length, SQLLEN *_str_len_or_ind_ptr)
{
    STMT_ENTRY(_statement_handle);
    if (_parameter_number < 1) {
        mapepire_diag_add(&s->h, "07009", 0, "Invalid descriptor index");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_input_output_type != SQL_PARAM_INPUT && _input_output_type != SQL_PARAM_OUTPUT &&
        _input_output_type != SQL_PARAM_INPUT_OUTPUT) {
        mapepire_diag_add(&s->h, "HY105", 0, "Invalid parameter type");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (!mapepire_ctype_valid(_value_type)) {
        mapepire_diag_add(&s->h, "HY003", 0, "Program type out of range");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (!mapepire_sqltype_valid(_parameter_type)) {
        mapepire_diag_add(&s->h, "HY004", 0, "Invalid SQL data type");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_buffer_length < 0) {
        mapepire_diag_add(&s->h, "HY090", 0, "Invalid string or buffer length");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_parameter_number > s->npbind) {
        mapepire_parambind *np = (mapepire_parambind *)realloc(s->pbind, _parameter_number * sizeof *np);
        if (!np) {
            mapepire_diag_add(&s->h, "HY001", 0, "Out of memory");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        memset(np + s->npbind, 0, (_parameter_number - (size_t)s->npbind) * sizeof *np);
        s->pbind = np;
        s->npbind = _parameter_number;
    }
    mapepire_parambind *pb = &s->pbind[_parameter_number - 1];
    pb->bound = 1;
    pb->io = _input_output_type;
    pb->ctype = _value_type;
    pb->sqltype = _parameter_type;
    pb->colsize = _column_size;
    pb->digits = _decimal_digits;
    pb->ptr = _parameter_value_ptr;
    pb->buflen = _buffer_length;
    pb->ind = _str_len_or_ind_ptr;
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLDescribeParam(SQLHSTMT _statement_handle, SQLUSMALLINT _parameter_number,
                                             SQLSMALLINT *_data_type_ptr, SQLULEN *_parameter_size_ptr,
                                             SQLSMALLINT *_decimal_digits_ptr, SQLSMALLINT *_nullable_ptr)
{
    STMT_ENTRY(_statement_handle);
    SQLRETURN rc = describe(s);
    if (!SQL_SUCCEEDED(rc)) return mapepire_ret(&s->h, rc);
    if (_parameter_number < 1 || _parameter_number > s->npdesc) {
        mapepire_diag_add(&s->h, "07009", 0, "Invalid descriptor index");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    mapepire_paramdesc *p = &s->pdesc[_parameter_number - 1];
    if (_data_type_ptr) *_data_type_ptr = p->sql_type;
    if (_parameter_size_ptr) *_parameter_size_ptr = p->size;
    if (_decimal_digits_ptr) *_decimal_digits_ptr = p->digits;
    if (_nullable_ptr) *_nullable_ptr = p->nullable;
    return SQL_SUCCESS;
}

/* ---- API: result metadata ------------------------------------------------------------ */

/* Columns of the current result, or of the prepared statement. */
static SQLRETURN current_cols(mapepire_stmt *_s, mapepire_col **_cols, int *_n)
{
    if (_s->state == MAPEPIRE_ST_EXECUTED) {
        *_cols = _s->cols;
        *_n = _s->has_result ? _s->ncols : 0;
        return SQL_SUCCESS;
    }
    if (_s->state == MAPEPIRE_ST_PREPARED) {
        SQLRETURN rc = describe(_s);
        if (!SQL_SUCCEEDED(rc)) return rc;
        *_cols = _s->pcols;
        *_n = _s->npcols;
        return SQL_SUCCESS;
    }
    mapepire_diag_add(&_s->h, "HY010", 0, "Function sequence error: no statement has been prepared or executed");
    return SQL_ERROR;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT _statement_handle, SQLSMALLINT *_column_count)
{
    STMT_ENTRY(_statement_handle);
    mapepire_col *cols;
    int n;
    SQLRETURN rc = current_cols(s, &cols, &n);
    if (!SQL_SUCCEEDED(rc)) return mapepire_ret(&s->h, rc);
    if (_column_count) *_column_count = (SQLSMALLINT)n;
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLDescribeColW(SQLHSTMT _statement_handle, SQLUSMALLINT _column_number, SQLWCHAR *_column_name,
                                            SQLSMALLINT _buffer_length, SQLSMALLINT *_name_length,
                                            SQLSMALLINT *_data_type, SQLULEN *_column_size, SQLSMALLINT *_decimal_digits,
                                            SQLSMALLINT *_nullable)
{
    STMT_ENTRY(_statement_handle);
    mapepire_col *cols;
    int n;
    SQLRETURN rc = current_cols(s, &cols, &n);
    if (!SQL_SUCCEEDED(rc)) return mapepire_ret(&s->h, rc);
    if (_column_number < 1 || _column_number > n) {
        mapepire_diag_add(&s->h, "07009", 0, "Invalid descriptor index");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    mapepire_col *c = &cols[_column_number - 1];
    if (_data_type) *_data_type = c->sql_type;
    if (_column_size) *_column_size = c->size;
    if (_decimal_digits) *_decimal_digits = c->digits;
    if (_nullable) *_nullable = c->nullable;
    if (mapepire_to_w_chars(c->name, _column_name, _buffer_length, _name_length)) {
        mapepire_diag_add(&s->h, "01004", 0, "String data, right truncated");
        return mapepire_ret(&s->h, SQL_SUCCESS_WITH_INFO);
    }
    return SQL_SUCCESS;
}

static const char *literal_prefix(SQLSMALLINT _t)
{
    if (mapepire_sqltype_is_char(_t)) return "'";
    if (mapepire_sqltype_is_binary(_t)) return "X'";
    if (_t == SQL_TYPE_DATE || _t == SQL_TYPE_TIME || _t == SQL_TYPE_TIMESTAMP) return "'";
    return "";
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLColAttributeW(SQLHSTMT _statement_handle, SQLUSMALLINT _column_number,
                                             SQLUSMALLINT _field_identifier, SQLPOINTER _character_attribute,
                                             SQLSMALLINT _buffer_length, SQLSMALLINT *_string_length,
#if defined(_WIN64) || !defined(_WIN32)
                                             SQLLEN *_numeric_attribute
#else
                                             SQLPOINTER _numeric_attribute
#endif
)
{
    STMT_ENTRY(_statement_handle);
    mapepire_col *cols;
    int n;
    SQLRETURN rc = current_cols(s, &cols, &n);
    if (!SQL_SUCCEEDED(rc)) return mapepire_ret(&s->h, rc);
    SQLLEN num = 0;
    const char *str = NULL;
    if (_field_identifier == SQL_DESC_COUNT || _field_identifier == SQL_COLUMN_COUNT) {
        num = n;
    } else {
        if (_column_number < 1 || _column_number > n) {
            mapepire_diag_add(&s->h, "07009", 0, "Invalid descriptor index");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        mapepire_col *c = &cols[_column_number - 1];
        switch (_field_identifier) {
        case SQL_DESC_NAME:
        case SQL_COLUMN_NAME: str = c->name; break;
        case SQL_DESC_LABEL: str = c->label; break;
        case SQL_DESC_BASE_COLUMN_NAME: str = c->name; break;
        case SQL_DESC_TYPE_NAME: str = c->type_name; break;
        case SQL_DESC_LOCAL_TYPE_NAME: str = c->type_name; break;
        case SQL_DESC_TABLE_NAME:
        case SQL_DESC_BASE_TABLE_NAME: str = c->table ? c->table : ""; break;
        case SQL_DESC_SCHEMA_NAME: str = c->schema ? c->schema : ""; break;
        case SQL_DESC_CATALOG_NAME: str = ""; break;
        case SQL_DESC_LITERAL_PREFIX: str = literal_prefix(c->sql_type); break;
        case SQL_DESC_LITERAL_SUFFIX: str = *literal_prefix(c->sql_type) ? "'" : ""; break;
        case SQL_DESC_CONCISE_TYPE: /* == SQL_COLUMN_TYPE */ num = c->sql_type; break;
        case SQL_DESC_TYPE: num = mapepire_type_verbose(c->sql_type); break;
        case SQL_DESC_DATETIME_INTERVAL_CODE: num = mapepire_type_datetime_sub(c->sql_type); break;
        case SQL_DESC_LENGTH: num = (SQLLEN)c->size; break;
        case SQL_COLUMN_LENGTH: num = c->octet; break;
        case SQL_DESC_OCTET_LENGTH: num = c->octet; break;
        case SQL_DESC_PRECISION:
            num = (c->sql_type == SQL_TYPE_TIMESTAMP || c->sql_type == SQL_TYPE_TIME) ? c->digits : (SQLLEN)c->size;
            break;
        case SQL_COLUMN_PRECISION: num = (SQLLEN)c->size; break;
        case SQL_DESC_SCALE:
        case SQL_COLUMN_SCALE: num = c->digits; break;
        case SQL_DESC_DISPLAY_SIZE: num = c->display; break;
        case SQL_DESC_NULLABLE:
        case SQL_COLUMN_NULLABLE: num = c->nullable; break;
        case SQL_DESC_UNSIGNED: num = mapepire_sqltype_is_numeric(c->sql_type) ? (c->is_unsigned ? SQL_TRUE : SQL_FALSE) : SQL_TRUE; break;
        case SQL_DESC_AUTO_UNIQUE_VALUE: num = c->autoinc ? SQL_TRUE : SQL_FALSE; break;
        case SQL_DESC_CASE_SENSITIVE: num = c->case_sensitive ? SQL_TRUE : SQL_FALSE; break;
        case SQL_DESC_SEARCHABLE: num = c->searchable; break;
        case SQL_DESC_UPDATABLE: num = c->updatable; break;
        case SQL_DESC_FIXED_PREC_SCALE: num = SQL_FALSE; break;
        case SQL_DESC_NUM_PREC_RADIX: num = c->radix; break;
        case SQL_DESC_UNNAMED: num = (c->name && *c->name) ? SQL_NAMED : SQL_UNNAMED; break;
        default:
            mapepire_diag_add(&s->h, "HY091", 0, "Invalid descriptor field identifier (%u)", (unsigned)_field_identifier);
            return mapepire_ret(&s->h, SQL_ERROR);
        }
    }
    if (str) {
        if (mapepire_to_w_bytes_small(str, _character_attribute, _buffer_length, _string_length) && _character_attribute) {
            mapepire_diag_add(&s->h, "01004", 0, "String data, right truncated");
            return mapepire_ret(&s->h, SQL_SUCCESS_WITH_INFO);
        }
        return SQL_SUCCESS;
    }
    if (_numeric_attribute) *(SQLLEN *)_numeric_attribute = num;
    return SQL_SUCCESS;
}

/* ---- API: fetching ---------------------------------------------------------------------- */

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLBindCol(SQLHSTMT _statement_handle, SQLUSMALLINT _column_number, SQLSMALLINT _target_type,
                                       SQLPOINTER _target_value, SQLLEN _buffer_length, SQLLEN *_str_len_or_ind)
{
    STMT_ENTRY(_statement_handle);
    if (_column_number < 1) {
        mapepire_diag_add(&s->h, "07009", 0, "Bookmark columns are not supported");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (!_target_value && !_str_len_or_ind) {
        /* unbind */
        if (_column_number <= s->ncbind) memset(&s->cbind[_column_number - 1], 0, sizeof s->cbind[0]);
        return SQL_SUCCESS;
    }
    if (!mapepire_ctype_valid(_target_type)) {
        mapepire_diag_add(&s->h, "HY003", 0, "Program type out of range");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_buffer_length < 0) {
        mapepire_diag_add(&s->h, "HY090", 0, "Invalid string or buffer length");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_column_number > s->ncbind) {
        mapepire_colbind *nb = (mapepire_colbind *)realloc(s->cbind, _column_number * sizeof *nb);
        if (!nb) {
            mapepire_diag_add(&s->h, "HY001", 0, "Out of memory");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        memset(nb + s->ncbind, 0, (_column_number - (size_t)s->ncbind) * sizeof *nb);
        s->cbind = nb;
        s->ncbind = _column_number;
    }
    mapepire_colbind *b = &s->cbind[_column_number - 1];
    b->ctype = _target_type;
    b->ptr = _target_value;
    b->buflen = _buffer_length;
    b->ind = _str_len_or_ind;
    return SQL_SUCCESS;
}

static SQLRETURN bind_row(mapepire_stmt *_s, const mpjson *_row, SQLULEN _i)
{
    SQLRETURN rc = SQL_SUCCESS;
    if (_s->retrieve_data == SQL_RD_OFF) return rc;
    SQLLEN offset = _s->row_bind_offset ? (SQLLEN)*_s->row_bind_offset : 0;
    for (int c = 0; c < _s->ncbind && c < _s->ncols; c++) {
        mapepire_colbind *b = &_s->cbind[c];
        if (!b->ptr && !b->ind) continue;
        SQLSMALLINT ct = b->ctype == SQL_C_DEFAULT ? mapepire_default_ctype(_s->cols[c].sql_type) : b->ctype;
        char *target = (char *)b->ptr;
        SQLLEN *ind = b->ind;
        if (_s->row_bind_type == SQL_BIND_BY_COLUMN) {
            SQLLEN elem = mapepire_ctype_size(ct);
            if (!elem) elem = b->buflen;
            if (target) target += offset + (SQLLEN)_i * elem;
            if (ind) ind = (SQLLEN *)((char *)ind + offset) + _i;
        } else {
            SQLLEN stride = (SQLLEN)_s->row_bind_type;
            if (target) target += offset + (SQLLEN)_i * stride;
            if (ind) ind = (SQLLEN *)((char *)ind + offset + (SQLLEN)_i * stride);
        }
        SQLRETURN r = mapepire_cell_to_c(_s, &_s->cols[c], row_cell(_s, _row, c), ct, target, b->buflen, ind,
                                   (SQLLEN)_i + 1, c + 1, NULL);
        if (r == SQL_ERROR) rc = SQL_ERROR;
        else if (r == SQL_SUCCESS_WITH_INFO && rc == SQL_SUCCESS) rc = SQL_SUCCESS_WITH_INFO;
    }
    return rc;
}

static SQLRETURN fetch_rowset(mapepire_stmt *_s)
{
    if (!_s->has_result) {
        mapepire_diag_add(&_s->h, "24000", 0, "Invalid cursor state: no result set is open");
        return SQL_ERROR;
    }
    gd_reset(_s);
    mpjson_free(_s->prev_resp); /* nothing points into it once a new rowset starts */
    _s->prev_resp = NULL;
    SQLULEN size = _s->row_array_size ? _s->row_array_size : 1;
    SQLULEN got = 0;
    int warnings = 0, errors = 0;
    SQLRETURN fail = SQL_SUCCESS;
    for (SQLULEN i = 0; i < size; i++) {
        mpjson *row;
        SQLRETURN r = next_row(_s, &row);
        if (r == SQL_NO_DATA) break;
        if (r == SQL_ERROR) { fail = SQL_ERROR; break; }
        _s->cur_row = row;
        SQLRETURN br = bind_row(_s, row, i);
        if (_s->row_status) _s->row_status[i] = br == SQL_ERROR ? SQL_ROW_ERROR
                                               : br == SQL_SUCCESS_WITH_INFO ? SQL_ROW_SUCCESS_WITH_INFO
                                                                             : SQL_ROW_SUCCESS;
        if (br == SQL_ERROR) errors++;
        else if (br == SQL_SUCCESS_WITH_INFO) warnings++;
        got++;
    }
    if (_s->row_status)
        for (SQLULEN i = got; i < size; i++) _s->row_status[i] = SQL_ROW_NOROW;
    if (_s->rows_fetched) *_s->rows_fetched = got;
    if (fail == SQL_ERROR && !got) return SQL_ERROR;
    if (!got) {
        _s->cur_row = NULL;
        return SQL_NO_DATA;
    }
    if (errors && size == 1) return SQL_ERROR;
    if (errors || warnings || fail == SQL_ERROR) return SQL_SUCCESS_WITH_INFO;
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLFetch(SQLHSTMT _statement_handle)
{
    STMT_ENTRY(_statement_handle);
    return mapepire_ret(&s->h, fetch_rowset(s));
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLFetchScroll(SQLHSTMT _statement_handle, SQLSMALLINT _fetch_orientation, SQLLEN _fetch_offset)
{
    (void)_fetch_offset;
    STMT_ENTRY(_statement_handle);
    if (_fetch_orientation != SQL_FETCH_NEXT) {
        mapepire_diag_add(&s->h, "HY106", 0, "Fetch type out of range: the cursor is forward-only");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    return mapepire_ret(&s->h, fetch_rowset(s));
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetData(SQLHSTMT _statement_handle, SQLUSMALLINT _column_number, SQLSMALLINT _target_type,
                                       SQLPOINTER _target_value, SQLLEN _buffer_length, SQLLEN *_str_len_or_ind_ptr)
{
    STMT_ENTRY(_statement_handle);
    if (!s->has_result || !s->cur_row) {
        mapepire_diag_add(&s->h, "24000", 0, "Invalid cursor state: no current row");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_column_number < 1 || _column_number > s->ncols) {
        mapepire_diag_add(&s->h, "07009", 0, "Invalid descriptor index");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (!mapepire_ctype_valid(_target_type) && _target_type != SQL_ARD_TYPE) {
        mapepire_diag_add(&s->h, "HY003", 0, "Program type out of range");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_target_type == SQL_ARD_TYPE) {
        _target_type = _column_number <= s->ncbind && s->cbind[_column_number - 1].ctype ? s->cbind[_column_number - 1].ctype
                                                                                    : SQL_C_DEFAULT;
    }
    if (s->gd_col != _column_number || s->gd_ctype != _target_type) {
        gd_reset(s);
        s->gd_col = _column_number;
        s->gd_ctype = _target_type;
    }
    mapepire_getdata gd = {&s->gd_cache, &s->gd_off, &s->gd_started, &s->gd_finished};
    SQLRETURN rc = mapepire_cell_to_c(s, &s->cols[_column_number - 1], row_cell(s, s->cur_row, _column_number - 1), _target_type,
                                _target_value, _buffer_length, _str_len_or_ind_ptr, 1, _column_number, &gd);
    return mapepire_ret(&s->h, rc);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLRowCount(SQLHSTMT _statement_handle, SQLLEN *_row_count)
{
    STMT_ENTRY(_statement_handle);
    if (s->state != MAPEPIRE_ST_EXECUTED) {
        mapepire_diag_add(&s->h, "HY010", 0, "Function sequence error");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_row_count) *_row_count = s->row_count;
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLMoreResults(SQLHSTMT _statement_handle)
{
    STMT_ENTRY(_statement_handle);
    /* Mapepire returns a single result per request. */
    if (s->state == MAPEPIRE_ST_EXECUTED) {
        mapepire_stmt_close_cursor(s);
        mapepire_cols_free(s->cols, s->ncols);
        s->cols = NULL;
        s->ncols = 0;
        s->row_count = -1;
    }
    return SQL_NO_DATA;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLCloseCursor(SQLHSTMT _statement_handle)
{
    STMT_ENTRY(_statement_handle);
    if (!s->has_result) {
        mapepire_diag_add(&s->h, "24000", 0, "Invalid cursor state: no cursor is open");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    mapepire_stmt_close_cursor(s);
    s->state = s->sql ? MAPEPIRE_ST_PREPARED : MAPEPIRE_ST_ALLOC;
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLCancel(SQLHSTMT _statement_handle)
{
    STMT_ENTRY(_statement_handle);
    if (s->state == MAPEPIRE_ST_NEED_DATA) {
        s->state = MAPEPIRE_ST_PREPARED;
        s->dae_current = -1;
        for (int i = 0; i < s->npbind; i++) {
            s->pbind[i].dae = 0;
            mapepire_buf_clear(&s->pbind[i].dae_data);
        }
    }
    /* Statements execute synchronously; there is nothing else to cancel. */
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT _statement_handle, SQLUSMALLINT _option)
{
    STMT_ENTRY(_statement_handle);
    switch (_option) {
    case SQL_CLOSE:
        if (s->has_result) {
            mapepire_stmt_close_cursor(s);
            s->state = s->sql ? MAPEPIRE_ST_PREPARED : MAPEPIRE_ST_ALLOC;
        }
        return SQL_SUCCESS;
    case SQL_UNBIND:
        free(s->cbind);
        s->cbind = NULL;
        s->ncbind = 0;
        return SQL_SUCCESS;
    case SQL_RESET_PARAMS:
        if (s->state == MAPEPIRE_ST_NEED_DATA) {
            mapepire_diag_add(&s->h, "HY010", 0, "Function sequence error: data-at-execution parameters are pending");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        free_pbind(s);
        return SQL_SUCCESS;
    case SQL_DROP:
        mapepire_stmt_destroy(s);
        return SQL_SUCCESS;
    default:
        mapepire_diag_add(&s->h, "HY092", 0, "Invalid attribute/option identifier");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetCursorNameW(SQLHSTMT _statement_handle, SQLWCHAR *_cursor_name, SQLSMALLINT _buffer_length,
                                              SQLSMALLINT *_name_length_ptr)
{
    STMT_ENTRY(_statement_handle);
    if (mapepire_to_w_chars(s->cursor_name, _cursor_name, _buffer_length, _name_length_ptr)) {
        mapepire_diag_add(&s->h, "01004", 0, "String data, right truncated");
        return mapepire_ret(&s->h, SQL_SUCCESS_WITH_INFO);
    }
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLSetCursorNameW(SQLHSTMT _statement_handle, SQLWCHAR *_cursor_name, SQLSMALLINT _name_length)
{
    STMT_ENTRY(_statement_handle);
    char *n = mapepire_from_w(_cursor_name, _name_length);
    if (!n || !*n || strlen(n) >= sizeof s->cursor_name) {
        free(n);
        mapepire_diag_add(&s->h, "34000", 0, "Invalid cursor name");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    mapepire_snprintf(s->cursor_name, sizeof s->cursor_name, "%s", n);
    free(n);
    return SQL_SUCCESS;
}

/* ---- API: statement attributes -------------------------------------------------------- */

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLSetStmtAttrW(SQLHSTMT _statement_handle, SQLINTEGER _attribute, SQLPOINTER _value,
                                            SQLINTEGER _string_length)
{
    (void)_string_length;
    STMT_ENTRY(_statement_handle);
    SQLULEN v = (SQLULEN)_value;
    switch (_attribute) {
    case SQL_ATTR_ROW_ARRAY_SIZE:
    case SQL_ROWSET_SIZE:
        if (v == 0) {
            mapepire_diag_add(&s->h, "HY024", 0, "Invalid attribute value");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        s->row_array_size = v;
        break;
    case SQL_ATTR_ROW_BIND_TYPE: s->row_bind_type = v; break;
    case SQL_ATTR_ROW_BIND_OFFSET_PTR: s->row_bind_offset = (SQLULEN *)_value; break;
    case SQL_ATTR_ROW_STATUS_PTR: s->row_status = (SQLUSMALLINT *)_value; break;
    case SQL_ATTR_ROWS_FETCHED_PTR: s->rows_fetched = (SQLULEN *)_value; break;
    case SQL_ATTR_ROW_OPERATION_PTR: s->row_operation = (SQLUSMALLINT *)_value; break;
    case SQL_ATTR_PARAMSET_SIZE:
        if (v == 0) {
            mapepire_diag_add(&s->h, "HY024", 0, "Invalid attribute value");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        s->paramset_size = v;
        break;
    case SQL_ATTR_PARAM_BIND_TYPE: s->param_bind_type = v; break;
    case SQL_ATTR_PARAM_BIND_OFFSET_PTR: s->param_bind_offset = (SQLULEN *)_value; break;
    case SQL_ATTR_PARAM_STATUS_PTR: s->param_status = (SQLUSMALLINT *)_value; break;
    case SQL_ATTR_PARAMS_PROCESSED_PTR: s->params_processed = (SQLULEN *)_value; break;
    case SQL_ATTR_PARAM_OPERATION_PTR: s->param_operation = (SQLUSMALLINT *)_value; break;
    case SQL_ATTR_MAX_ROWS: s->max_rows = v; break;
    case SQL_ATTR_MAX_LENGTH: s->max_length = v; break;
    case SQL_ATTR_QUERY_TIMEOUT: s->query_timeout = v; break;
    case SQL_ATTR_NOSCAN: s->noscan = v; break;
    case SQL_ATTR_RETRIEVE_DATA: s->retrieve_data = v; break;
    case SQL_ATTR_METADATA_ID: s->metadata_id = v; break;
    case SQL_ATTR_KEYSET_SIZE: s->keyset_size = v; break;
    case SQL_ATTR_SIMULATE_CURSOR: break;
    case SQL_ATTR_CURSOR_TYPE:
        if (v != SQL_CURSOR_FORWARD_ONLY) {
            mapepire_diag_add(&s->h, "01S02", 0, "Option value changed: only forward-only cursors are supported");
            return mapepire_ret(&s->h, SQL_SUCCESS_WITH_INFO);
        }
        break;
    case SQL_ATTR_CONCURRENCY:
        if (v != SQL_CONCUR_READ_ONLY) {
            mapepire_diag_add(&s->h, "01S02", 0, "Option value changed: only read-only cursors are supported");
            return mapepire_ret(&s->h, SQL_SUCCESS_WITH_INFO);
        }
        break;
    case SQL_ATTR_CURSOR_SCROLLABLE:
        if (v != SQL_NONSCROLLABLE) {
            mapepire_diag_add(&s->h, "HYC00", 0, "Scrollable cursors are not supported");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        break;
    case SQL_ATTR_CURSOR_SENSITIVITY:
        if (v != SQL_UNSPECIFIED && v != SQL_INSENSITIVE) {
            mapepire_diag_add(&s->h, "HYC00", 0, "Sensitive cursors are not supported");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        break;
    case SQL_ATTR_USE_BOOKMARKS:
        if (v != SQL_UB_OFF) {
            mapepire_diag_add(&s->h, "HYC00", 0, "Bookmarks are not supported");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        break;
    case SQL_ATTR_ASYNC_ENABLE:
        if (v != SQL_ASYNC_ENABLE_OFF) {
            mapepire_diag_add(&s->h, "HYC00", 0, "Asynchronous execution is not supported");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        break;
    case SQL_ATTR_ENABLE_AUTO_IPD:
        if (v != SQL_FALSE) {
            mapepire_diag_add(&s->h, "HYC00", 0, "Automatic IPD population is not supported");
            return mapepire_ret(&s->h, SQL_ERROR);
        }
        break;
    case SQL_ATTR_APP_ROW_DESC:
    case SQL_ATTR_APP_PARAM_DESC:
        if (_value == SQL_NULL_HANDLE || _value == &s->ard || _value == &s->apd) break;
        mapepire_diag_add(&s->h, "HYC00", 0, "Explicit descriptors are not supported");
        return mapepire_ret(&s->h, SQL_ERROR);
    case SQL_ATTR_IMP_ROW_DESC:
    case SQL_ATTR_IMP_PARAM_DESC:
        mapepire_diag_add(&s->h, "HY017", 0, "Invalid use of an automatically allocated descriptor handle");
        return mapepire_ret(&s->h, SQL_ERROR);
    default:
        mapepire_diag_add(&s->h, "HY092", 0, "Invalid attribute/option identifier (%d)", (int)_attribute);
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetStmtAttrW(SQLHSTMT _statement_handle, SQLINTEGER _attribute, SQLPOINTER _value,
                                            SQLINTEGER _buffer_length, SQLINTEGER *_string_length)
{
    (void)_buffer_length;
    STMT_ENTRY(_statement_handle);
    SQLULEN v = 0;
    SQLPOINTER p = NULL;
    int is_ptr = 0;
    switch (_attribute) {
    case SQL_ATTR_APP_ROW_DESC: p = &s->ard; is_ptr = 1; break;
    case SQL_ATTR_APP_PARAM_DESC: p = &s->apd; is_ptr = 1; break;
    case SQL_ATTR_IMP_ROW_DESC: p = &s->ird; is_ptr = 1; break;
    case SQL_ATTR_IMP_PARAM_DESC: p = &s->ipd; is_ptr = 1; break;
    case SQL_ATTR_ROW_BIND_OFFSET_PTR: p = s->row_bind_offset; is_ptr = 1; break;
    case SQL_ATTR_ROW_STATUS_PTR: p = s->row_status; is_ptr = 1; break;
    case SQL_ATTR_ROWS_FETCHED_PTR: p = s->rows_fetched; is_ptr = 1; break;
    case SQL_ATTR_ROW_OPERATION_PTR: p = s->row_operation; is_ptr = 1; break;
    case SQL_ATTR_PARAM_BIND_OFFSET_PTR: p = s->param_bind_offset; is_ptr = 1; break;
    case SQL_ATTR_PARAM_STATUS_PTR: p = s->param_status; is_ptr = 1; break;
    case SQL_ATTR_PARAMS_PROCESSED_PTR: p = s->params_processed; is_ptr = 1; break;
    case SQL_ATTR_PARAM_OPERATION_PTR: p = s->param_operation; is_ptr = 1; break;
    case SQL_ATTR_FETCH_BOOKMARK_PTR: p = NULL; is_ptr = 1; break;
    case SQL_ATTR_ROW_ARRAY_SIZE:
    case SQL_ROWSET_SIZE: v = s->row_array_size; break;
    case SQL_ATTR_ROW_BIND_TYPE: v = s->row_bind_type; break;
    case SQL_ATTR_PARAMSET_SIZE: v = s->paramset_size; break;
    case SQL_ATTR_PARAM_BIND_TYPE: v = s->param_bind_type; break;
    case SQL_ATTR_MAX_ROWS: v = s->max_rows; break;
    case SQL_ATTR_MAX_LENGTH: v = s->max_length; break;
    case SQL_ATTR_QUERY_TIMEOUT: v = s->query_timeout; break;
    case SQL_ATTR_NOSCAN: v = s->noscan; break;
    case SQL_ATTR_RETRIEVE_DATA: v = s->retrieve_data; break;
    case SQL_ATTR_METADATA_ID: v = s->metadata_id; break;
    case SQL_ATTR_KEYSET_SIZE: v = s->keyset_size; break;
    case SQL_ATTR_CURSOR_TYPE: v = SQL_CURSOR_FORWARD_ONLY; break;
    case SQL_ATTR_CONCURRENCY: v = SQL_CONCUR_READ_ONLY; break;
    case SQL_ATTR_CURSOR_SCROLLABLE: v = SQL_NONSCROLLABLE; break;
    case SQL_ATTR_CURSOR_SENSITIVITY: v = SQL_INSENSITIVE; break;
    case SQL_ATTR_USE_BOOKMARKS: v = SQL_UB_OFF; break;
    case SQL_ATTR_ASYNC_ENABLE: v = SQL_ASYNC_ENABLE_OFF; break;
    case SQL_ATTR_ENABLE_AUTO_IPD: v = SQL_FALSE; break;
    case SQL_ATTR_SIMULATE_CURSOR: v = SQL_SC_UNIQUE; break;
    case SQL_ATTR_ROW_NUMBER:
        v = s->cur_row ? (SQLULEN)s->cur_row_number : 0;
        break;
    default:
        mapepire_diag_add(&s->h, "HY092", 0, "Invalid attribute/option identifier (%d)", (int)_attribute);
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (is_ptr) {
        if (_value) *(SQLPOINTER *)_value = p;
        if (_string_length) *_string_length = sizeof(SQLPOINTER);
    } else {
        if (_value) *(SQLULEN *)_value = v;
        if (_string_length) *_string_length = sizeof(SQLULEN);
    }
    return SQL_SUCCESS;
}

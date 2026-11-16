/*
 * Catalog functions, implemented with the SYSIBM ODBC/JDBC catalog views
 * that Db2 for i provides (SYSIBM.SQLTABLES, SYSIBM.SQLCOLUMNS, ...).
 * Search arguments are always passed as parameters, never spliced into SQL.
 */
#include "drv.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    mapepire_buf sql;
    mpjson   *params;
    int    nwhere;
    int    metadata_id;
} query;

static void q_init(query *_q, const char *_select, int _metadata_id)
{
    mapepire_buf_init(&_q->sql);
    mapepire_buf_append_str(&_q->sql, _select);
    _q->params = mpjson_new(MPJSON_ARRAY);
    _q->nwhere = 0;
    _q->metadata_id = _metadata_id;
}

static void q_free(query *_q)
{
    mapepire_buf_free(&_q->sql);
    mpjson_free(_q->params);
}

static void q_and(query *_q)
{
    mapepire_buf_append_str(&_q->sql, _q->nwhere++ ? " AND " : " WHERE ");
}

/* Identifier argument (SQL_ATTR_METADATA_ID = TRUE): quoted keeps case, else upper-case. */
static char *ident_value(const char *_v)
{
    size_t n = strlen(_v);
    if (n >= 2 && _v[0] == '"' && _v[n - 1] == '"') {
        mapepire_buf b;
        mapepire_buf_init(&b);
        for (size_t i = 1; i + 1 < n; i++) {
            if (_v[i] == '"' && _v[i + 1] == '"') i++;
            mapepire_buf_append_char(&b, _v[i]);
        }
        return mapepire_buf_detach(&b);
    }
    char *u = mapepire_strdup(_v);
    mapepire_trim(u);
    for (char *p = u; *p; p++) *p = (char)toupper((unsigned char)*p);
    return u;
}

/* Exact-match argument (ordinary argument or identifier). NULL = no filter. */
static void q_exact(query *_q, const char *_col, const char *_v)
{
    if (!_v) return;
    char *val = _q->metadata_id ? ident_value(_v) : mapepire_strdup(_v);
    q_and(_q);
    mapepire_buf_printf(&_q->sql, "%s = ?", _col);
    mpjson_push(_q->params, mpjson_new_string(val));
    free(val);
}

/* Pattern value argument. NULL or "%" = no filter. */
static void q_pattern(query *_q, const char *_col, const char *_v)
{
    if (!_v) return;
    if (_q->metadata_id) {
        q_exact(_q, _col, _v);
        return;
    }
    if (!strcmp(_v, "%")) return;
    q_and(_q);
    if (strpbrk(_v, "%_\\")) {
        mapepire_buf_printf(&_q->sql, "%s LIKE ? ESCAPE '\\'", _col);
    } else {
        mapepire_buf_printf(&_q->sql, "%s = ?", _col);
    }
    mpjson_push(_q->params, mpjson_new_string(_v));
}

/* Catalog arguments: an empty string and NULL both mean "the current database". */
static void q_catalog(query *_q, const char *_col, const char *_v)
{
    if (!_v || !*_v) return;
    q_exact(_q, _col, _v);
}

static SQLRETURN q_run(mapepire_stmt *_s, query *_q, const char *_order_by)
{
    if (_order_by) {
        mapepire_buf_append_str(&_q->sql, " ORDER BY ");
        mapepire_buf_append_str(&_q->sql, _order_by);
    }
    mpjson *params = _q->params;
    _q->params = NULL;
    SQLRETURN rc = mapepire_stmt_run_query(_s, _q->sql.data, params);
    q_free(_q);
    return rc;
}

static SQLRETURN begin(mapepire_stmt *_s)
{
    if (_s->has_result) {
        mapepire_diag_add(&_s->h, "24000", 0, "Invalid cursor state");
        return SQL_ERROR;
    }
    if (_s->state == MAPEPIRE_ST_NEED_DATA) {
        mapepire_diag_add(&_s->h, "HY010", 0, "Function sequence error");
        return SQL_ERROR;
    }
    free(_s->sql);
    _s->sql = NULL;
    _s->described = 0;
    mapepire_stmt_reset_result(_s);
    return SQL_SUCCESS;
}

static void set_fixup(mapepire_stmt *_s, int _kind, int _data_col, int _type_col, int _sqltype_col, int _sub_col)
{
    if (!_s->has_result) return;
    _s->fixup = _kind;
    _s->fixup_data_col = _data_col;
    _s->fixup_type_col = _type_col;
    _s->fixup_sqltype_col = _sqltype_col;
    _s->fixup_sub_col = _sub_col;
}

/*
 * ODBC defines the result set of every catalog function, so the column types
 * are set from the specification rather than from the server's metadata
 * (which, e.g., describes CAST(NULL AS VARCHAR(n)) differently on some
 * servers).  spec: V = VARCHAR(128), L = VARCHAR(2000), S = SMALLINT,
 * I = INTEGER, one letter per column.
 */
static void spec_types(mapepire_stmt *_s, const char *_spec)
{
    if (!_s->has_result) return;
    for (int i = 0; i < _s->ncols && _spec[i]; i++) {
        mapepire_col *c = &_s->cols[i];
        const char *tn = _spec[i] == 'S' ? "SMALLINT" : _spec[i] == 'I' ? "INTEGER" : "VARCHAR";
        long len = _spec[i] == 'L' ? 2000 : 128;
        free(c->type_name);
        c->type_name = mapepire_strdup(tn);
        mapepire_type_describe(tn, len, 0, c);
        if (c->nullable != SQL_NO_NULLS) c->nullable = SQL_NULLABLE;
    }
}

#define CAT_ENTRY(_hnd)                                       \
    if (!MAPEPIRE_VALID(_hnd, MAPEPIRE_H_STMT)) return SQL_INVALID_HANDLE; \
    mapepire_stmt *s = (mapepire_stmt *)(_hnd);                          \
    mapepire_diag_clear(&s->h);                                   \
    if (!SQL_SUCCEEDED(begin(s))) return mapepire_ret(&s->h, SQL_ERROR)

#define ARG(_name, _w, _l) char *_name = mapepire_from_w(_w, _l)

/* ---- SQLTables -------------------------------------------------------------------- */

static mpjson *row_strings(int _n, const char *const *_vals)
{
    mpjson *r = mpjson_new(MPJSON_ARRAY);
    for (int i = 0; i < _n; i++) mpjson_push(r, _vals[i] ? mpjson_new_string(_vals[i]) : mpjson_new(MPJSON_NULL));
    return r;
}

static const char *const TABLES_COLS[] = {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS"};
static const SQLSMALLINT TABLES_TYPES[] = {SQL_VARCHAR, SQL_VARCHAR, SQL_VARCHAR, SQL_VARCHAR, SQL_VARCHAR};

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLTablesW(SQLHSTMT _statement_handle, SQLWCHAR *_catalog_name, SQLSMALLINT _name_length1,
                                       SQLWCHAR *_schema_name, SQLSMALLINT _name_length2, SQLWCHAR *_table_name,
                                       SQLSMALLINT _name_length3, SQLWCHAR *_table_type, SQLSMALLINT _name_length4)
{
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(tab, _table_name, _name_length3);
    ARG(typ, _table_type, _name_length4);
    SQLRETURN rc;
    int empty_sch = sch && !*sch, empty_tab = tab && !*tab, empty_cat = cat && !*cat;

    if (cat && !strcmp(cat, SQL_ALL_CATALOGS) && empty_sch && empty_tab) {
        const char *vals[5] = {mapepire_dbc_catalog(s->dbc), NULL, NULL, NULL, NULL};
        mpjson *rows = mpjson_new(MPJSON_ARRAY);
        mpjson_push(rows, row_strings(5, vals));
        mapepire_stmt_local_result(s, TABLES_COLS, TABLES_TYPES, 5, rows);
        rc = SQL_SUCCESS;
    } else if (sch && !strcmp(sch, SQL_ALL_SCHEMAS) && empty_cat && empty_tab) {
        query q;
        q_init(&q,
               "SELECT TABLE_CAT, TABLE_SCHEM, CAST(NULL AS VARCHAR(128)) AS TABLE_NAME, "
               "CAST(NULL AS VARCHAR(128)) AS TABLE_TYPE, CAST(NULL AS VARCHAR(254)) AS REMARKS "
               "FROM SYSIBM.SQLSCHEMAS",
               0);
        rc = q_run(s, &q, "TABLE_SCHEM");
        spec_types(s, "VVVVL");
    } else if (typ && !strcmp(typ, SQL_ALL_TABLE_TYPES) && empty_cat && empty_sch && empty_tab) {
        static const char *const kinds[] = {"ALIAS", "MATERIALIZED QUERY TABLE", "SYSTEM TABLE", "TABLE", "VIEW"};
        mpjson *rows = mpjson_new(MPJSON_ARRAY);
        for (int i = 0; i < 5; i++) {
            const char *vals[5] = {NULL, NULL, NULL, kinds[i], NULL};
            mpjson_push(rows, row_strings(5, vals));
        }
        mapepire_stmt_local_result(s, TABLES_COLS, TABLES_TYPES, 5, rows);
        rc = SQL_SUCCESS;
    } else {
        query q;
        q_init(&q,
               "SELECT TABLE_CAT, TABLE_SCHEM, TABLE_NAME, TABLE_TYPE, "
               "CAST(COALESCE(REMARKS, NULLIF(TABLE_TEXT, '')) AS VARCHAR(2000)) AS REMARKS FROM SYSIBM.SQLTABLES",
               (int)s->metadata_id);
        q_catalog(&q, "TABLE_CAT", cat);
        q_pattern(&q, "TABLE_SCHEM", sch);
        q_pattern(&q, "TABLE_NAME", tab);
        if (typ && *typ && strcmp(typ, "%")) {
            /* comma separated list, values optionally in single quotes */
            int n = 0;
            mapepire_buf in;
            mapepire_buf_init(&in);
            char *list = mapepire_strdup(typ);
            for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
                mapepire_trim(tok);
                size_t l = strlen(tok);
                if (l >= 2 && tok[0] == '\'' && tok[l - 1] == '\'') {
                    tok[l - 1] = '\0';
                    tok++;
                }
                if (!*tok) continue;
                mapepire_buf_append_str(&in, n++ ? ", ?" : "?");
                mpjson_push(q.params, mpjson_new_string(tok));
            }
            free(list);
            if (n) {
                q_and(&q);
                mapepire_buf_printf(&q.sql, "TABLE_TYPE IN (%s)", in.data);
            }
            mapepire_buf_free(&in);
        }
        rc = q_run(s, &q, "TABLE_TYPE, TABLE_CAT, TABLE_SCHEM, TABLE_NAME");
        spec_types(s, "VVVVL");
    }
    free(cat); free(sch); free(tab); free(typ);
    return mapepire_ret(&s->h, rc);
}

/* ---- SQLColumns ------------------------------------------------------------------- */

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLColumnsW(SQLHSTMT _statement_handle, SQLWCHAR *_catalog_name, SQLSMALLINT _name_length1,
                                        SQLWCHAR *_schema_name, SQLSMALLINT _name_length2, SQLWCHAR *_table_name,
                                        SQLSMALLINT _name_length3, SQLWCHAR *_column_name, SQLSMALLINT _name_length4)
{
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(tab, _table_name, _name_length3);
    ARG(col, _column_name, _name_length4);
    query q;
    q_init(&q,
           "SELECT TABLE_CAT, TABLE_SCHEM, TABLE_NAME, COLUMN_NAME, DATA_TYPE, TYPE_NAME, COLUMN_SIZE, "
           "BUFFER_LENGTH, DECIMAL_DIGITS, NUM_PREC_RADIX, NULLABLE, "
           "CAST(COALESCE(REMARKS, NULLIF(COLUMN_TEXT, '')) AS VARCHAR(2000)) AS REMARKS, COLUMN_DEF, SQL_DATA_TYPE, SQL_DATETIME_SUB, "
           "CHAR_OCTET_LENGTH, ORDINAL_POSITION, IS_NULLABLE FROM SYSIBM.SQLCOLUMNS",
           (int)s->metadata_id);
    q_catalog(&q, "TABLE_CAT", cat);
    q_pattern(&q, "TABLE_SCHEM", sch);
    q_pattern(&q, "TABLE_NAME", tab);
    q_pattern(&q, "COLUMN_NAME", col);
    SQLRETURN rc = q_run(s, &q, "TABLE_CAT, TABLE_SCHEM, TABLE_NAME, ORDINAL_POSITION");
    set_fixup(s, MAPEPIRE_FIX_COLUMNS, 4, 5, 13, 14);
    spec_types(s, "VVVVSVIISSSLLSSIIV");
    free(cat); free(sch); free(tab); free(col);
    return mapepire_ret(&s->h, rc);
}

/* ---- keys and indexes -------------------------------------------------------------- */

static SQLRETURN require(mapepire_stmt *_s, const char *_v, const char *_what)
{
    if (_v) return SQL_SUCCESS;
    mapepire_diag_add(&_s->h, "HY009", 0, "Invalid use of null pointer: %s is required", _what);
    return SQL_ERROR;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLPrimaryKeysW(SQLHSTMT _statement_handle, SQLWCHAR *_catalog_name, SQLSMALLINT _name_length1,
                                            SQLWCHAR *_schema_name, SQLSMALLINT _name_length2, SQLWCHAR *_table_name,
                                            SQLSMALLINT _name_length3)
{
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(tab, _table_name, _name_length3);
    SQLRETURN rc = require(s, tab, "the table name");
    if (SQL_SUCCEEDED(rc)) {
        query q;
        q_init(&q, "SELECT TABLE_CAT, TABLE_SCHEM, TABLE_NAME, COLUMN_NAME, KEY_SEQ, PK_NAME FROM SYSIBM.SQLPRIMARYKEYS",
               (int)s->metadata_id);
        q_catalog(&q, "TABLE_CAT", cat);
        q_exact(&q, "TABLE_SCHEM", sch && *sch ? sch : NULL);
        q_exact(&q, "TABLE_NAME", tab);
        rc = q_run(s, &q, "TABLE_CAT, TABLE_SCHEM, TABLE_NAME, KEY_SEQ");
        spec_types(s, "VVVVSV");
    }
    free(cat); free(sch); free(tab);
    return mapepire_ret(&s->h, rc);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLForeignKeysW(SQLHSTMT _statement_handle, SQLWCHAR *_pk_catalog_name, SQLSMALLINT _name_length1,
                                            SQLWCHAR *_pk_schema_name, SQLSMALLINT _name_length2, SQLWCHAR *_pk_table_name,
                                            SQLSMALLINT _name_length3, SQLWCHAR *_fk_catalog_name, SQLSMALLINT _name_length4,
                                            SQLWCHAR *_fk_schema_name, SQLSMALLINT _name_length5, SQLWCHAR *_fk_table_name,
                                            SQLSMALLINT _name_length6)
{
    CAT_ENTRY(_statement_handle);
    ARG(pkcat, _pk_catalog_name, _name_length1);
    ARG(pksch, _pk_schema_name, _name_length2);
    ARG(pktab, _pk_table_name, _name_length3);
    ARG(fkcat, _fk_catalog_name, _name_length4);
    ARG(fksch, _fk_schema_name, _name_length5);
    ARG(fktab, _fk_table_name, _name_length6);
    SQLRETURN rc;
    if (!(pktab && *pktab) && !(fktab && *fktab)) {
        mapepire_diag_add(&s->h, "HY009", 0, "Invalid use of null pointer: a primary key or foreign key table is required");
        rc = SQL_ERROR;
    } else {
        query q;
        q_init(&q,
               "SELECT PKTABLE_CAT, PKTABLE_SCHEM, PKTABLE_NAME, PKCOLUMN_NAME, FKTABLE_CAT, FKTABLE_SCHEM, "
               "FKTABLE_NAME, FKCOLUMN_NAME, KEY_SEQ, UPDATE_RULE, DELETE_RULE, FK_NAME, PK_NAME, DEFERRABILITY "
               "FROM SYSIBM.SQLFOREIGNKEYS",
               (int)s->metadata_id);
        q_catalog(&q, "PKTABLE_CAT", pkcat);
        q_exact(&q, "PKTABLE_SCHEM", pksch && *pksch ? pksch : NULL);
        q_exact(&q, "PKTABLE_NAME", pktab && *pktab ? pktab : NULL);
        q_catalog(&q, "FKTABLE_CAT", fkcat);
        q_exact(&q, "FKTABLE_SCHEM", fksch && *fksch ? fksch : NULL);
        q_exact(&q, "FKTABLE_NAME", fktab && *fktab ? fktab : NULL);
        rc = q_run(s, &q, pktab && *pktab ? "FKTABLE_CAT, FKTABLE_SCHEM, FKTABLE_NAME, KEY_SEQ"
                                         : "PKTABLE_CAT, PKTABLE_SCHEM, PKTABLE_NAME, KEY_SEQ");
        spec_types(s, "VVVVVVVVSSSVVS");
    }
    free(pkcat); free(pksch); free(pktab); free(fkcat); free(fksch); free(fktab);
    return mapepire_ret(&s->h, rc);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLStatisticsW(SQLHSTMT _statement_handle, SQLWCHAR *_catalog_name, SQLSMALLINT _name_length1,
                                           SQLWCHAR *_schema_name, SQLSMALLINT _name_length2, SQLWCHAR *_table_name,
                                           SQLSMALLINT _name_length3, SQLUSMALLINT _unique, SQLUSMALLINT _reserved)
{
    (void)_reserved;
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(tab, _table_name, _name_length3);
    SQLRETURN rc = require(s, tab, "the table name");
    if (SQL_SUCCEEDED(rc) && _unique != SQL_INDEX_UNIQUE && _unique != SQL_INDEX_ALL) {
        mapepire_diag_add(&s->h, "HY100", 0, "Uniqueness option type out of range");
        rc = SQL_ERROR;
    }
    if (SQL_SUCCEEDED(rc)) {
        query q;
        q_init(&q,
               "SELECT TABLE_CAT, TABLE_SCHEM, TABLE_NAME, NON_UNIQUE, INDEX_QUALIFIER, INDEX_NAME, TYPE, "
               "ORDINAL_POSITION, COLUMN_NAME, ASC_OR_DESC, CARDINALITY, PAGES, FILTER_CONDITION "
               "FROM SYSIBM.SQLSTATISTICS",
               (int)s->metadata_id);
        q_catalog(&q, "TABLE_CAT", cat);
        q_exact(&q, "TABLE_SCHEM", sch && *sch ? sch : NULL);
        q_exact(&q, "TABLE_NAME", tab);
        if (_unique == SQL_INDEX_UNIQUE) {
            q_and(&q);
            mapepire_buf_append_str(&q.sql, "(NON_UNIQUE = 0 OR TYPE = 0)");
        }
        rc = q_run(s, &q, "NON_UNIQUE, TYPE, INDEX_QUALIFIER, INDEX_NAME, ORDINAL_POSITION");
        spec_types(s, "VVVSVVSSVVIIL");
    }
    free(cat); free(sch); free(tab);
    return mapepire_ret(&s->h, rc);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLSpecialColumnsW(SQLHSTMT _statement_handle, SQLUSMALLINT _identifier_type,
                                               SQLWCHAR *_catalog_name, SQLSMALLINT _name_length1, SQLWCHAR *_schema_name,
                                               SQLSMALLINT _name_length2, SQLWCHAR *_table_name, SQLSMALLINT _name_length3,
                                               SQLUSMALLINT _scope, SQLUSMALLINT _nullable)
{
    (void)_scope;
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(tab, _table_name, _name_length3);
    SQLRETURN rc = require(s, tab, "the table name");
    if (SQL_SUCCEEDED(rc) && _identifier_type != SQL_BEST_ROWID && _identifier_type != SQL_ROWVER) {
        mapepire_diag_add(&s->h, "HY097", 0, "Column type out of range");
        rc = SQL_ERROR;
    }
    if (SQL_SUCCEEDED(rc) && _identifier_type == SQL_ROWVER) {
        static const char *const names[] = {"SCOPE", "COLUMN_NAME", "DATA_TYPE", "TYPE_NAME", "COLUMN_SIZE",
                                            "BUFFER_LENGTH", "DECIMAL_DIGITS", "PSEUDO_COLUMN"};
        static const SQLSMALLINT types[] = {SQL_SMALLINT, SQL_VARCHAR, SQL_SMALLINT, SQL_VARCHAR,
                                            SQL_INTEGER, SQL_INTEGER, SQL_SMALLINT, SQL_SMALLINT};
        mapepire_stmt_local_result(s, names, types, 8, NULL);
    } else if (SQL_SUCCEEDED(rc)) {
        query q;
        q_init(&q,
               "SELECT SCOPE, COLUMN_NAME, DATA_TYPE, TYPE_NAME, COLUMN_SIZE, BUFFER_LENGTH, DECIMAL_DIGITS, "
               "PSEUDO_COLUMN FROM SYSIBM.SQLSPECIALCOLUMNS",
               (int)s->metadata_id);
        q_catalog(&q, "TABLE_CAT", cat);
        q_exact(&q, "TABLE_SCHEM", sch && *sch ? sch : NULL);
        q_exact(&q, "TABLE_NAME", tab);
        if (_nullable == SQL_NO_NULLS) {
            q_and(&q);
            mapepire_buf_append_str(&q.sql, "NULLABLE <> 1");
        }
        rc = q_run(s, &q, "SCOPE");
        set_fixup(s, MAPEPIRE_FIX_SPECIAL, 2, 3, -1, -1);
        spec_types(s, "SVSVIISS");
    }
    free(cat); free(sch); free(tab);
    return mapepire_ret(&s->h, rc);
}

/* ---- procedures ------------------------------------------------------------------- */

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLProceduresW(SQLHSTMT _statement_handle, SQLWCHAR *_catalog_name, SQLSMALLINT _name_length1,
                                           SQLWCHAR *_schema_name, SQLSMALLINT _name_length2, SQLWCHAR *_proc_name,
                                           SQLSMALLINT _name_length3)
{
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(proc, _proc_name, _name_length3);
    query q;
    q_init(&q,
           "SELECT PROCEDURE_CAT, PROCEDURE_SCHEM, PROCEDURE_NAME, NUM_INPUT_PARAMS, NUM_OUTPUT_PARAMS, "
           "NUM_RESULT_SETS, REMARKS, PROCEDURE_TYPE FROM SYSIBM.SQLPROCEDURES",
           (int)s->metadata_id);
    q_catalog(&q, "PROCEDURE_CAT", cat);
    q_pattern(&q, "PROCEDURE_SCHEM", sch);
    q_pattern(&q, "PROCEDURE_NAME", proc);
    SQLRETURN rc = q_run(s, &q, "PROCEDURE_CAT, PROCEDURE_SCHEM, PROCEDURE_NAME");
    spec_types(s, "VVVIIILS");
    free(cat); free(sch); free(proc);
    return mapepire_ret(&s->h, rc);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLProcedureColumnsW(SQLHSTMT _statement_handle, SQLWCHAR *_catalog_name,
                                                 SQLSMALLINT _name_length1, SQLWCHAR *_schema_name,
                                                 SQLSMALLINT _name_length2, SQLWCHAR *_proc_name, SQLSMALLINT _name_length3,
                                                 SQLWCHAR *_column_name, SQLSMALLINT _name_length4)
{
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(proc, _proc_name, _name_length3);
    ARG(col, _column_name, _name_length4);
    query q;
    q_init(&q,
           "SELECT PROCEDURE_CAT, PROCEDURE_SCHEM, PROCEDURE_NAME, COLUMN_NAME, COLUMN_TYPE, DATA_TYPE, TYPE_NAME, "
           "COLUMN_SIZE, BUFFER_LENGTH, DECIMAL_DIGITS, NUM_PREC_RADIX, NULLABLE, REMARKS, COLUMN_DEF, "
           "SQL_DATA_TYPE, SQL_DATETIME_SUB, CHAR_OCTET_LENGTH, ORDINAL_POSITION, IS_NULLABLE "
           "FROM SYSIBM.SQLPROCEDURECOLS",
           (int)s->metadata_id);
    q_catalog(&q, "PROCEDURE_CAT", cat);
    q_pattern(&q, "PROCEDURE_SCHEM", sch);
    q_pattern(&q, "PROCEDURE_NAME", proc);
    q_pattern(&q, "COLUMN_NAME", col);
    SQLRETURN rc = q_run(s, &q, "PROCEDURE_CAT, PROCEDURE_SCHEM, PROCEDURE_NAME, COLUMN_TYPE, ORDINAL_POSITION");
    set_fixup(s, MAPEPIRE_FIX_PROCCOLS, 5, 6, 14, 15);
    spec_types(s, "VVVVSSVIISSSLLSSIIV");
    free(cat); free(sch); free(proc); free(col);
    return mapepire_ret(&s->h, rc);
}

/* ---- privileges ------------------------------------------------------------------- */

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLTablePrivilegesW(SQLHSTMT _statement_handle, SQLWCHAR *_catalog_name,
                                                SQLSMALLINT _name_length1, SQLWCHAR *_schema_name, SQLSMALLINT _name_length2,
                                                SQLWCHAR *_table_name, SQLSMALLINT _name_length3)
{
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(tab, _table_name, _name_length3);
    query q;
    q_init(&q,
           "SELECT TABLE_CAT, TABLE_SCHEM, TABLE_NAME, GRANTOR, GRANTEE, PRIVILEGE, IS_GRANTABLE "
           "FROM SYSIBM.SQLTABLEPRIVILEGES",
           (int)s->metadata_id);
    q_catalog(&q, "TABLE_CAT", cat);
    q_pattern(&q, "TABLE_SCHEM", sch);
    q_pattern(&q, "TABLE_NAME", tab);
    SQLRETURN rc = q_run(s, &q, "TABLE_CAT, TABLE_SCHEM, TABLE_NAME, PRIVILEGE, GRANTEE");
    spec_types(s, "VVVVVVV");
    free(cat); free(sch); free(tab);
    return mapepire_ret(&s->h, rc);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLColumnPrivilegesW(SQLHSTMT _statement_handle, SQLWCHAR *_catalog_name,
                                                 SQLSMALLINT _name_length1, SQLWCHAR *_schema_name,
                                                 SQLSMALLINT _name_length2, SQLWCHAR *_table_name, SQLSMALLINT _name_length3,
                                                 SQLWCHAR *_column_name, SQLSMALLINT _name_length4)
{
    CAT_ENTRY(_statement_handle);
    ARG(cat, _catalog_name, _name_length1);
    ARG(sch, _schema_name, _name_length2);
    ARG(tab, _table_name, _name_length3);
    ARG(col, _column_name, _name_length4);
    SQLRETURN rc = require(s, tab, "the table name");
    if (SQL_SUCCEEDED(rc)) {
        query q;
        q_init(&q,
               "SELECT TABLE_CAT, TABLE_SCHEM, TABLE_NAME, COLUMN_NAME, GRANTOR, GRANTEE, PRIVILEGE, IS_GRANTABLE "
               "FROM SYSIBM.SQLCOLPRIVILEGES",
               (int)s->metadata_id);
        q_catalog(&q, "TABLE_CAT", cat);
        q_exact(&q, "TABLE_SCHEM", sch && *sch ? sch : NULL);
        q_exact(&q, "TABLE_NAME", tab);
        q_pattern(&q, "COLUMN_NAME", col);
        rc = q_run(s, &q, "TABLE_CAT, TABLE_SCHEM, TABLE_NAME, COLUMN_NAME, PRIVILEGE");
        spec_types(s, "VVVVVVVV");
    }
    free(cat); free(sch); free(tab); free(col);
    return mapepire_ret(&s->h, rc);
}

/*
 * Internal declarations for the Mapepire ODBC driver.
 *
 * The driver is a Unicode ODBC 3.8 driver: every API taking or returning
 * strings is exported in its W form only, and the Driver Manager maps ANSI
 * applications onto it.  Strings are kept as UTF-8 internally.
 */
#ifndef MAPEPIRE_DRV_H
#define MAPEPIRE_DRV_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <sql.h>
#include <sqlext.h>
#include <sqlucode.h>

#include "mapepire_client.h"

#define MAPEPIRE_DRIVER_FILE     "mapepireodbc"
#define MAPEPIRE_DRIVER_VER      "00.01.0000"
#define MAPEPIRE_DRIVER_ODBC_VER "03.80"
#define MAPEPIRE_DBMS_NAME       "DB2/400 SQL"
#define MAPEPIRE_DEFAULT_PORT    8076
#define MAPEPIRE_DEFAULT_BLOCK   1000
#define MAPEPIRE_MSG_PREFIX      "[Mapepire][ODBC Driver]"
#define MAPEPIRE_SRV_PREFIX      "[Mapepire][ODBC Driver][Db2 for i]"

#ifdef _WIN32
#define MAPEPIRE_EXPORT
#else
#define MAPEPIRE_EXPORT __attribute__((visibility("default")))
#endif

/* ---- diagnostics -------------------------------------------------------- */

typedef struct mapepire_diag_rec {
    char        state[6];
    SQLINTEGER  native;
    char       *msg;
    SQLLEN      row;     /* SQL_ROW_NUMBER_UNKNOWN / SQL_NO_ROW_NUMBER or 1-based */
    SQLINTEGER  col;     /* SQL_COLUMN_NUMBER_UNKNOWN / SQL_NO_COLUMN_NUMBER or 1-based */
} mapepire_diag_rec;

typedef struct mapepire_diag {
    mapepire_diag_rec *recs;
    int          count;
    int          cap;
    SQLRETURN    rc;
} mapepire_diag;

enum { MAPEPIRE_H_ENV = 0x4d50454e, MAPEPIRE_H_DBC = 0x4d504442, MAPEPIRE_H_STMT = 0x4d505354, MAPEPIRE_H_DESC = 0x4d504445 };

typedef struct mapepire_env  mapepire_env;
typedef struct mapepire_dbc  mapepire_dbc;
typedef struct mapepire_stmt mapepire_stmt;
typedef struct mapepire_desc mapepire_desc;

/* Every handle starts with this header so the type can be checked. */
typedef struct mapepire_handle {
    int     magic;
    mapepire_diag diag;
} mapepire_handle;

struct mapepire_env {
    mapepire_handle   h;
    SQLINTEGER  odbc_version;
    SQLINTEGER  connection_pooling;
    SQLINTEGER  cp_match;
    mapepire_mutex    lock;
    mapepire_dbc     *dbcs;              /* linked list of connections */
};

/* ---- connection configuration ------------------------------------------- */

typedef struct mapepire_kv {
    char *key;    /* canonical upper-case keyword */
    char *value;
} mapepire_kv;

typedef struct mapepire_conf {
    mapepire_kv *items;
    int    count;
    int    cap;
} mapepire_conf;

void        mapepire_conf_free(mapepire_conf *_c);
const char *mapepire_conf_get(const mapepire_conf *_c, const char *_key);
void        mapepire_conf_set(mapepire_conf *_c, const char *_key, const char *_value);   /* key may be an alias */
/* Parse "k=v;k={v;x}" into c (later values override). 0 on success. */
int         mapepire_conf_parse(mapepire_conf *_c, const char *_connstr, char *_err, size_t _errlen);
/* Merge attributes stored for a DSN in odbc.ini (connection string wins). */
void        mapepire_conf_load_dsn(mapepire_conf *_c, const char *_dsn);
/* Build an output connection string. */
char       *mapepire_conf_to_string(const mapepire_conf *_c);
/* Resolve an alias to its canonical keyword, or NULL when unknown. */
const char *mapepire_conf_canonical(const char *_key);

/* ---- connection --------------------------------------------------------- */

enum { MAPEPIRE_CHARENC_UTF8 = 0, MAPEPIRE_CHARENC_ANSI = 1 };

struct mapepire_dbc {
    mapepire_handle   h;
    mapepire_env     *env;
    mapepire_dbc     *next;
    mapepire_client  *cli;
    mapepire_conf     conf;
    mapepire_mutex    lock;

    /* attributes */
    SQLUINTEGER autocommit;        /* SQL_AUTOCOMMIT_ON/OFF */
    SQLUINTEGER access_mode;
    SQLUINTEGER txn_isolation;     /* 0 = server default */
    SQLUINTEGER login_timeout;
    SQLUINTEGER connection_timeout;
    SQLUINTEGER metadata_id;
    int         ansi_app;          /* SQL_ATTR_ANSI_APP was set to TRUE */
    int         char_encoding;     /* encoding of SQL_C_CHAR data */

    /* effective settings */
    int         block_size;
    char       *server;
    char       *user;
    char       *dsn;
    char       *catalog;           /* lazily fetched CURRENT SERVER */
    int         catalog_fetched;
    char       *user_props;        /* PROPS keyword as given */

    mapepire_stmt    *stmts;             /* linked list */
};

/* ---- descriptors (minimal: handles exist so SQLGetStmtAttr works) -------- */

struct mapepire_desc {
    mapepire_handle h;
    mapepire_stmt  *stmt;
    int       kind;   /* SQL_ATTR_APP_ROW_DESC ... */
};

/* ---- statement ----------------------------------------------------------- */

typedef struct mapepire_col {
    char       *name;
    char       *label;
    char       *type_name;
    char       *table;
    char       *schema;
    SQLSMALLINT sql_type;      /* concise type */
    SQLULEN     size;          /* column size */
    SQLSMALLINT digits;        /* decimal digits */
    SQLSMALLINT nullable;
    SQLLEN      display;
    SQLLEN      octet;
    int         is_unsigned;
    int         autoinc;
    int         case_sensitive;
    int         searchable;
    int         updatable;
    int         radix;
} mapepire_col;

typedef struct mapepire_colbind {
    SQLSMALLINT ctype;
    SQLPOINTER  ptr;
    SQLLEN      buflen;
    SQLLEN     *ind;
} mapepire_colbind;

typedef struct mapepire_parambind {
    int         bound;
    SQLSMALLINT io;            /* SQL_PARAM_INPUT ... */
    SQLSMALLINT ctype;
    SQLSMALLINT sqltype;
    SQLULEN     colsize;
    SQLSMALLINT digits;
    SQLPOINTER  ptr;
    SQLLEN      buflen;
    SQLLEN     *ind;
    /* data-at-execution */
    int         dae;           /* needs data at exec in the current execution */
    int         dae_done;
    int         dae_null;
    mapepire_buf      dae_data;
} mapepire_parambind;

typedef struct mapepire_paramdesc {
    char       *type_name;
    SQLSMALLINT sql_type;
    SQLULEN     size;
    SQLSMALLINT digits;
    SQLSMALLINT nullable;
    int         mode;          /* SQL_PARAM_INPUT ... */
} mapepire_paramdesc;

enum { MAPEPIRE_FIX_NONE = 0, MAPEPIRE_FIX_COLUMNS, MAPEPIRE_FIX_PROCCOLS, MAPEPIRE_FIX_SPECIAL };

enum { MAPEPIRE_ST_ALLOC = 0, MAPEPIRE_ST_PREPARED, MAPEPIRE_ST_EXECUTED, MAPEPIRE_ST_NEED_DATA };

struct mapepire_stmt {
    mapepire_handle    h;
    mapepire_dbc      *dbc;
    mapepire_stmt     *next;
    mapepire_desc      ard, apd, ird, ipd;  /* implicit descriptor stubs */

    char        *sql;             /* UTF-8 */
    int          state;
    int          nmarkers;        /* lexical count of parameter markers */
    char         cursor_name[32];

    /* prepared metadata (from prepare_sql, fetched lazily) */
    int          described;
    mapepire_col      *pcols;
    int          npcols;
    mapepire_paramdesc *pdesc;
    int          npdesc;

    /* current result */
    int          has_result;      /* a result set is open */
    mapepire_col      *cols;
    int          ncols;
    mpjson         *resp;            /* owns block */
    mpjson         *prev_resp;       /* previous block, kept until the rowset is complete */
    mpjson         *block;           /* data array within resp */
    size_t       pos;             /* next row index in block */
    int          is_done;
    int          terse;
    char         cursor[32];      /* server cursor id ("" when none/local) */
    SQLLEN       row_count;
    long long    rows_returned;
    mpjson         *cur_row;         /* current row (within block) */
    long long    cur_row_number;  /* 1-based number of the current row */
    int          fixup;           /* MAPEPIRE_FIX_* for catalog results */
    int          fixup_type_col;  /* 0-based index of TYPE_NAME */
    int          fixup_data_col;  /* 0-based index of DATA_TYPE */
    int          fixup_sqltype_col;
    int          fixup_sub_col;
    int          more_results_done;

    /* SQLGetData state */
    int          gd_col;          /* column of the last SQLGetData (1-based), 0 = none */
    SQLSMALLINT  gd_ctype;
    mapepire_buf       gd_cache;
    size_t       gd_off;
    int          gd_started;
    int          gd_finished;

    /* bindings */
    mapepire_colbind  *cbind;
    int          ncbind;
    mapepire_parambind *pbind;
    int          npbind;
    int          dae_current;     /* index of param being supplied, -1 none */

    /* attributes */
    SQLULEN      row_array_size;
    SQLULEN      row_bind_type;
    SQLULEN     *row_bind_offset;
    SQLUSMALLINT *row_status;
    SQLULEN     *rows_fetched;
    SQLULEN      paramset_size;
    SQLULEN      param_bind_type;
    SQLULEN     *param_bind_offset;
    SQLUSMALLINT *param_status;
    SQLULEN     *params_processed;
    SQLUSMALLINT *param_operation;
    SQLUSMALLINT *row_operation;
    SQLULEN      max_rows;
    SQLULEN      max_length;
    SQLULEN      query_timeout;
    SQLULEN      noscan;
    SQLULEN      retrieve_data;
    SQLULEN      metadata_id;
    SQLULEN      cursor_type;
    SQLULEN      concurrency;
    SQLULEN      keyset_size;
    SQLULEN      use_bookmarks;
};

/* ---- diag API ----------------------------------------------------------- */

void      mapepire_diag_clear(mapepire_handle *_h);
void      mapepire_diag_add(mapepire_handle *_h, const char *_state, SQLINTEGER _native, const char *_fmt, ...);
void      mapepire_diag_add_rc(mapepire_handle *_h, const char *_state, SQLINTEGER _native, SQLLEN _row, SQLINTEGER _col,
                         const char *_fmt, ...);
/* Record an mapepire_error as a diagnostic.  Returns SQL_ERROR. */
SQLRETURN mapepire_diag_from_error(mapepire_handle *_h, const mapepire_error *_e);
void      mapepire_diag_free(mapepire_diag *_d);
/* Set the return code for SQL_DIAG_RETURNCODE and return it. */
SQLRETURN mapepire_ret(mapepire_handle *_h, SQLRETURN _rc);

/* ---- string helpers ------------------------------------------------------ */

/* Convert an input SQLWCHAR string to UTF-8 (NULL stays NULL). */
char     *mapepire_from_w(const SQLWCHAR *_s, SQLINTEGER _len);
/* Copy UTF-8 into SQLWCHAR output buffers (lengths in characters or bytes).
 * Return 1 when the value was truncated (caller posts 01004). */
int       mapepire_to_w_chars(const char *_s, SQLWCHAR *_buf, SQLSMALLINT _cap, SQLSMALLINT *_outlen);
int       mapepire_to_w_bytes(const char *_s, SQLPOINTER _buf, SQLINTEGER _cap, SQLINTEGER *_outlen);
int       mapepire_to_w_bytes_small(const char *_s, SQLPOINTER _buf, SQLSMALLINT _cap, SQLSMALLINT *_outlen);

/* ---- types (drv_types.c) -------------------------------------------------- */

/* Fill col's ODBC attributes from a Db2 type name / precision / scale.
 * Returns 0 when the type name was not recognised (VARCHAR is assumed). */
int         mapepire_type_describe(const char *_type_name, long _precision, long _scale, mapepire_col *_col);
SQLSMALLINT mapepire_type_verbose(SQLSMALLINT _concise);
/* ODBC 2 applications get SQL_DATE/SQL_TIME/SQL_TIMESTAMP codes in result sets. */
SQLSMALLINT mapepire_odbc2_type(mapepire_dbc *_d, SQLSMALLINT _concise);
SQLSMALLINT mapepire_type_datetime_sub(SQLSMALLINT _concise);
SQLSMALLINT mapepire_default_ctype(SQLSMALLINT _sql_type);
SQLLEN      mapepire_ctype_size(SQLSMALLINT _ctype);    /* 0 for variable-length types */
int         mapepire_sqltype_is_binary(SQLSMALLINT _t);
int         mapepire_sqltype_is_numeric(SQLSMALLINT _t);
int         mapepire_sqltype_is_char(SQLSMALLINT _t);
int         mapepire_ctype_valid(SQLSMALLINT _ctype);
int         mapepire_sqltype_valid(SQLSMALLINT _t);

/* Convert a result cell to the application's C type. */
typedef struct mapepire_getdata {
    mapepire_buf *cache;
    size_t *off;
    int    *started;
    int    *finished;
} mapepire_getdata;

SQLRETURN mapepire_cell_to_c(mapepire_stmt *_s, const mapepire_col *_col, const mpjson *_cell, SQLSMALLINT _ctype, SQLPOINTER _target,
                       SQLLEN _buflen, SQLLEN *_ind, SQLLEN _row, SQLINTEGER _colno, mapepire_getdata *_gd);

/* Convert one application parameter value to JSON. *out NULL means SQL NULL. */
SQLRETURN mapepire_param_to_json(mapepire_stmt *_s, int _pidx, SQLULEN _row, mpjson **_out);

/* Decimal text -> plain notation (no exponent). Returns 0 on success. */
int       mapepire_number_normalize(const char *_in, char *_out, size_t _cap);

/* ---- connection helpers (drv_conn.c) --------------------------------------- */

const char *mapepire_dbc_catalog(mapepire_dbc *_d);
int         mapepire_dbc_connected(mapepire_dbc *_d);
/* Encode/decode SQL_C_CHAR data according to the connection's encoding. */
char       *mapepire_dbc_char_to_utf8(mapepire_dbc *_d, const char *_s, size_t _len, size_t *_outlen);
int         mapepire_dbc_utf8_to_char(mapepire_dbc *_d, const char *_s, size_t _len, mapepire_buf *_out);

/* ---- statement helpers (drv_stmt.c) ---------------------------------------- */

void      mapepire_stmt_close_cursor(mapepire_stmt *_s);
void      mapepire_stmt_reset_result(mapepire_stmt *_s);
SQLRETURN mapepire_stmt_run_query(mapepire_stmt *_s, const char *_sql, mpjson *_params);
/* Install a locally built result set (takes ownership of rows, an array of arrays). */
void      mapepire_stmt_local_result(mapepire_stmt *_s, const char *const *_names, const SQLSMALLINT *_types, int _ncols, mpjson *_rows);
void      mapepire_cols_free(mapepire_col *_c, int _n);
int       mapepire_cols_from_meta(const mpjson *_meta, mapepire_col **_out, int *_n);
int       mapepire_count_markers(const char *_sql);

/* Lock helpers for API entry points */
#define MAPEPIRE_VALID(_hnd, _mg) ((_hnd) != NULL && ((mapepire_handle *)(_hnd))->magic == (_mg))

#endif

/*
 * SQLGetInfoW, SQLGetFunctions and SQLGetTypeInfoW.
 */
#include "drv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- SQLGetInfoW --------------------------------------------------------------------- */

static void dbms_version(mapepire_dbc *_d, char *_out, size_t _cap)
{
    int a = 0, b = 0, c = 0;
    const char *v = _d->cli ? mapepire_client_version(_d->cli) : "";
    sscanf(v ? v : "", "%d.%d.%d", &a, &b, &c);
    mapepire_snprintf(_out, _cap, "%02d.%02d.%04d", a, b, c);
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetInfoW(SQLHDBC _connection_handle, SQLUSMALLINT _info_type, SQLPOINTER _info_value,
                                        SQLSMALLINT _buffer_length, SQLSMALLINT *_string_length)
{
    if (!MAPEPIRE_VALID(_connection_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
    mapepire_dbc *d = (mapepire_dbc *)_connection_handle;
    mapepire_diag_clear(&d->h);

    const char *str = NULL;
    char tmp[64];
    int kind = 0; /* 1 = USMALLINT, 2 = UINTEGER */
    SQLUINTEGER u = 0;

#define STR(_x) do { str = (_x); } while (0)
#define U16(_x) do { kind = 1; u = (SQLUINTEGER)(_x); } while (0)
#define U32(_x) do { kind = 2; u = (SQLUINTEGER)(_x); } while (0)

    switch (_info_type) {
    /* driver / data source */
    case SQL_DRIVER_NAME:
#ifdef _WIN32
        STR(MAPEPIRE_DRIVER_FILE ".dll");
#else
        STR("lib" MAPEPIRE_DRIVER_FILE ".so");
#endif
        break;
    case SQL_DRIVER_VER: STR(MAPEPIRE_DRIVER_VER); break;
    case SQL_DRIVER_ODBC_VER: STR(MAPEPIRE_DRIVER_ODBC_VER); break;
    case SQL_DBMS_NAME: STR(MAPEPIRE_DBMS_NAME); break;
    case SQL_DBMS_VER: dbms_version(d, tmp, sizeof tmp); STR(tmp); break;
    case SQL_SERVER_NAME: STR(d->server ? d->server : ""); break;
    case SQL_DATA_SOURCE_NAME: STR(d->dsn ? d->dsn : ""); break;
    case SQL_DATABASE_NAME: STR(mapepire_dbc_catalog(d)); break;
    case SQL_USER_NAME: STR(d->user ? d->user : ""); break;
    case SQL_DATA_SOURCE_READ_ONLY: STR(d->access_mode == SQL_MODE_READ_ONLY ? "Y" : "N"); break;
    case SQL_ACCESSIBLE_PROCEDURES: STR("Y"); break;
    case SQL_ACCESSIBLE_TABLES: STR("Y"); break;
    case SQL_ODBC_INTERFACE_CONFORMANCE: U32(SQL_OIC_CORE); break;
    case SQL_SQL_CONFORMANCE: U32(SQL_SC_SQL92_ENTRY); break;
    case SQL_ODBC_SQL_CONFORMANCE: U16(SQL_OSC_CORE); break;
    case SQL_ODBC_API_CONFORMANCE: U16(SQL_OAC_LEVEL1); break;
    case SQL_ODBC_SAG_CLI_CONFORMANCE: U16(SQL_OSCC_COMPLIANT); break;
    case SQL_STANDARD_CLI_CONFORMANCE: U32(SQL_SCC_ISO92_CLI); break;
    case SQL_XOPEN_CLI_YEAR: STR("1995"); break;

    /* limits */
    case SQL_ACTIVE_ENVIRONMENTS: U16(0); break;
    case SQL_MAX_DRIVER_CONNECTIONS: U16(0); break;
    case SQL_MAX_CONCURRENT_ACTIVITIES: U16(0); break;
    case SQL_MAX_ASYNC_CONCURRENT_STATEMENTS: U32(0); break;
    case SQL_MAX_CATALOG_NAME_LEN: U16(18); break;
    case SQL_MAX_SCHEMA_NAME_LEN: U16(128); break;
    case SQL_MAX_TABLE_NAME_LEN: U16(128); break;
    case SQL_MAX_COLUMN_NAME_LEN: U16(128); break;
    case SQL_MAX_CURSOR_NAME_LEN: U16(18); break;
    case SQL_MAX_PROCEDURE_NAME_LEN: U16(128); break;
    case SQL_MAX_IDENTIFIER_LEN: U16(128); break;
    case SQL_MAX_USER_NAME_LEN: U16(10); break;
    case SQL_MAX_COLUMNS_IN_GROUP_BY: U16(8000); break;
    case SQL_MAX_COLUMNS_IN_INDEX: U16(120); break;
    case SQL_MAX_COLUMNS_IN_ORDER_BY: U16(10000); break;
    case SQL_MAX_COLUMNS_IN_SELECT: U16(8000); break;
    case SQL_MAX_COLUMNS_IN_TABLE: U16(8000); break;
    case SQL_MAX_TABLES_IN_SELECT: U16(1000); break;
    case SQL_MAX_BINARY_LITERAL_LEN: U32(32672); break;
    case SQL_MAX_CHAR_LITERAL_LEN: U32(32740); break;
    case SQL_MAX_INDEX_SIZE: U32(32767); break;
    case SQL_MAX_ROW_SIZE: U32(32766); break;
    case SQL_MAX_ROW_SIZE_INCLUDES_LONG: STR("N"); break;
    case SQL_MAX_STATEMENT_LEN: U32(2097152); break;

    /* identifiers and terms */
    case SQL_IDENTIFIER_CASE: U16(SQL_IC_UPPER); break;
    case SQL_QUOTED_IDENTIFIER_CASE: U16(SQL_IC_SENSITIVE); break;
    case SQL_IDENTIFIER_QUOTE_CHAR: STR("\""); break;
    case SQL_CATALOG_NAME_SEPARATOR: STR("."); break;
    case SQL_CATALOG_NAME: STR("Y"); break;
    case SQL_CATALOG_TERM: STR("database"); break;
    case SQL_CATALOG_LOCATION: U16(SQL_CL_START); break;
    case SQL_CATALOG_USAGE: U32(SQL_CU_DML_STATEMENTS); break;
    case SQL_SCHEMA_TERM: STR("schema"); break;
    case SQL_SCHEMA_USAGE:
        U32(SQL_SU_DML_STATEMENTS | SQL_SU_PROCEDURE_INVOCATION | SQL_SU_TABLE_DEFINITION | SQL_SU_INDEX_DEFINITION |
            SQL_SU_PRIVILEGE_DEFINITION);
        break;
    case SQL_TABLE_TERM: STR("table"); break;
    case SQL_PROCEDURE_TERM: STR("procedure"); break;
    case SQL_PROCEDURES: STR("Y"); break;
    case SQL_SEARCH_PATTERN_ESCAPE: STR("\\"); break;
    case SQL_LIKE_ESCAPE_CLAUSE: STR("Y"); break;
    case SQL_SPECIAL_CHARACTERS: STR("@#$"); break;
    case SQL_KEYWORDS: STR(""); break;
    case SQL_COLLATION_SEQ: STR(""); break;

    /* SQL capabilities */
    case SQL_COLUMN_ALIAS: STR("Y"); break;
    case SQL_CORRELATION_NAME: U16(SQL_CN_ANY); break;
    case SQL_EXPRESSIONS_IN_ORDERBY: STR("Y"); break;
    case SQL_ORDER_BY_COLUMNS_IN_SELECT: STR("N"); break;
    case SQL_GROUP_BY: U16(SQL_GB_GROUP_BY_CONTAINS_SELECT); break;
    case SQL_INTEGRITY: STR("Y"); break;
    case SQL_NON_NULLABLE_COLUMNS: U16(SQL_NNC_NON_NULL); break;
    case SQL_NULL_COLLATION: U16(SQL_NC_HIGH); break;
    case SQL_CONCAT_NULL_BEHAVIOR: U16(SQL_CB_NULL); break;
    case SQL_OUTER_JOINS: STR("Y"); break;
    case SQL_OJ_CAPABILITIES:
        U32(SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL | SQL_OJ_NESTED | SQL_OJ_NOT_ORDERED | SQL_OJ_INNER |
            SQL_OJ_ALL_COMPARISON_OPS);
        break;
    case SQL_SUBQUERIES:
        U32(SQL_SQ_CORRELATED_SUBQUERIES | SQL_SQ_COMPARISON | SQL_SQ_EXISTS | SQL_SQ_IN | SQL_SQ_QUANTIFIED);
        break;
    case SQL_UNION: U32(SQL_U_UNION | SQL_U_UNION_ALL); break;
    case SQL_AGGREGATE_FUNCTIONS:
        U32(SQL_AF_ALL | SQL_AF_AVG | SQL_AF_COUNT | SQL_AF_DISTINCT | SQL_AF_MAX | SQL_AF_MIN | SQL_AF_SUM);
        break;
    case SQL_DATETIME_LITERALS: U32(SQL_DL_SQL92_DATE | SQL_DL_SQL92_TIME | SQL_DL_SQL92_TIMESTAMP); break;
    case SQL_CONVERT_FUNCTIONS: U32(SQL_FN_CVT_CAST); break;
    case SQL_NUMERIC_FUNCTIONS:
        U32(SQL_FN_NUM_ABS | SQL_FN_NUM_ACOS | SQL_FN_NUM_ASIN | SQL_FN_NUM_ATAN | SQL_FN_NUM_ATAN2 |
            SQL_FN_NUM_CEILING | SQL_FN_NUM_COS | SQL_FN_NUM_COT | SQL_FN_NUM_DEGREES | SQL_FN_NUM_EXP |
            SQL_FN_NUM_FLOOR | SQL_FN_NUM_LOG | SQL_FN_NUM_LOG10 | SQL_FN_NUM_MOD | SQL_FN_NUM_PI | SQL_FN_NUM_POWER |
            SQL_FN_NUM_RADIANS | SQL_FN_NUM_RAND | SQL_FN_NUM_ROUND | SQL_FN_NUM_SIGN | SQL_FN_NUM_SIN |
            SQL_FN_NUM_SQRT | SQL_FN_NUM_TAN | SQL_FN_NUM_TRUNCATE);
        break;
    case SQL_STRING_FUNCTIONS:
        U32(SQL_FN_STR_ASCII | SQL_FN_STR_CHAR | SQL_FN_STR_CONCAT | SQL_FN_STR_DIFFERENCE | SQL_FN_STR_INSERT |
            SQL_FN_STR_LCASE | SQL_FN_STR_LEFT | SQL_FN_STR_LENGTH | SQL_FN_STR_LOCATE | SQL_FN_STR_LTRIM |
            SQL_FN_STR_REPEAT | SQL_FN_STR_REPLACE | SQL_FN_STR_RIGHT | SQL_FN_STR_RTRIM | SQL_FN_STR_SOUNDEX |
            SQL_FN_STR_SPACE | SQL_FN_STR_SUBSTRING | SQL_FN_STR_UCASE);
        break;
    case SQL_SYSTEM_FUNCTIONS: U32(SQL_FN_SYS_DBNAME | SQL_FN_SYS_IFNULL | SQL_FN_SYS_USERNAME); break;
    case SQL_TIMEDATE_FUNCTIONS:
        U32(SQL_FN_TD_CURDATE | SQL_FN_TD_CURTIME | SQL_FN_TD_DAYNAME | SQL_FN_TD_DAYOFMONTH | SQL_FN_TD_DAYOFWEEK |
            SQL_FN_TD_DAYOFYEAR | SQL_FN_TD_HOUR | SQL_FN_TD_MINUTE | SQL_FN_TD_MONTH | SQL_FN_TD_MONTHNAME |
            SQL_FN_TD_NOW | SQL_FN_TD_QUARTER | SQL_FN_TD_SECOND | SQL_FN_TD_WEEK | SQL_FN_TD_YEAR |
            SQL_FN_TD_CURRENT_DATE | SQL_FN_TD_CURRENT_TIME | SQL_FN_TD_CURRENT_TIMESTAMP);
        break;
    case SQL_TIMEDATE_ADD_INTERVALS:
    case SQL_TIMEDATE_DIFF_INTERVALS: U32(0); break;
    case SQL_ALTER_TABLE:
        U32(SQL_AT_ADD_COLUMN_DEFAULT | SQL_AT_ADD_COLUMN_SINGLE | SQL_AT_ADD_CONSTRAINT | SQL_AT_DROP_COLUMN_CASCADE |
            SQL_AT_DROP_COLUMN_RESTRICT | SQL_AT_DROP_TABLE_CONSTRAINT_CASCADE | SQL_AT_DROP_TABLE_CONSTRAINT_RESTRICT);
        break;
    case SQL_INSERT_STATEMENT: U32(SQL_IS_INSERT_LITERALS | SQL_IS_INSERT_SEARCHED | SQL_IS_SELECT_INTO); break;
    case SQL_DESCRIBE_PARAMETER: STR("Y"); break;
    case SQL_MULT_RESULT_SETS: STR("N"); break;
    case SQL_MULTIPLE_ACTIVE_TXN: STR("Y"); break;
    case SQL_NEED_LONG_DATA_LEN: STR("N"); break;
    case SQL_ROW_UPDATES: STR("N"); break;
    case SQL_INFO_SCHEMA_VIEWS: U32(0); break;
    case SQL_DDL_INDEX: U32(SQL_DI_CREATE_INDEX | SQL_DI_DROP_INDEX); break;
    case SQL_INDEX_KEYWORDS: U32(SQL_IK_ASC | SQL_IK_DESC); break;
    case SQL_FILE_USAGE: U16(SQL_FILE_NOT_SUPPORTED); break;
    case SQL_DTC_TRANSITION_COST: U32(0); break;

    /* transactions */
    case SQL_TXN_CAPABLE: U16(SQL_TC_ALL); break;
    case SQL_DEFAULT_TXN_ISOLATION: U32(SQL_TXN_READ_UNCOMMITTED); break;
    case SQL_TXN_ISOLATION_OPTION:
        U32(SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED | SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE);
        break;
    case SQL_CURSOR_COMMIT_BEHAVIOR: U16(SQL_CB_PRESERVE); break;
    case SQL_CURSOR_ROLLBACK_BEHAVIOR: U16(SQL_CB_PRESERVE); break;

    /* cursors and fetching */
    case SQL_GETDATA_EXTENSIONS: U32(SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BOUND); break;
    case SQL_SCROLL_OPTIONS: U32(SQL_SO_FORWARD_ONLY); break;
    case SQL_SCROLL_CONCURRENCY: U32(SQL_SCCO_READ_ONLY); break;
    case SQL_FETCH_DIRECTION: U32(SQL_FD_FETCH_NEXT); break;
    case SQL_CURSOR_SENSITIVITY: U32(SQL_INSENSITIVE); break;
    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1: U32(SQL_CA1_NEXT); break;
    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2: U32(SQL_CA2_READ_ONLY_CONCURRENCY | SQL_CA2_MAX_ROWS_SELECT); break;
    case SQL_STATIC_CURSOR_ATTRIBUTES1:
    case SQL_STATIC_CURSOR_ATTRIBUTES2:
    case SQL_KEYSET_CURSOR_ATTRIBUTES1:
    case SQL_KEYSET_CURSOR_ATTRIBUTES2:
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES1:
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES2: U32(0); break;
    case SQL_STATIC_SENSITIVITY: U32(0); break;
    case SQL_POS_OPERATIONS: U32(0); break;
    case SQL_POSITIONED_STATEMENTS: U32(0); break;
    case SQL_LOCK_TYPES: U32(0); break;
    case SQL_BOOKMARK_PERSISTENCE: U32(0); break;
    case SQL_ASYNC_MODE: U32(SQL_AM_NONE); break;
#ifdef SQL_ASYNC_DBC_FUNCTIONS
    case SQL_ASYNC_DBC_FUNCTIONS: U32(SQL_ASYNC_DBC_NOT_CAPABLE); break;
#endif
#ifdef SQL_ASYNC_NOTIFICATION
    case SQL_ASYNC_NOTIFICATION: U32(SQL_ASYNC_NOTIFICATION_NOT_CAPABLE); break;
#endif
    case SQL_BATCH_ROW_COUNT: U32(SQL_BRC_ROLLED_UP); break;
    case SQL_BATCH_SUPPORT: U32(0); break;
    case SQL_PARAM_ARRAY_ROW_COUNTS: U32(SQL_PARC_NO_BATCH); break;
    case SQL_PARAM_ARRAY_SELECTS: U32(SQL_PAS_NO_SELECT); break;

    /* conversions: no CONVERT() support is advertised */
    case SQL_CONVERT_BIGINT: case SQL_CONVERT_BINARY: case SQL_CONVERT_BIT: case SQL_CONVERT_CHAR:
    case SQL_CONVERT_DATE: case SQL_CONVERT_DECIMAL: case SQL_CONVERT_DOUBLE: case SQL_CONVERT_FLOAT:
    case SQL_CONVERT_INTEGER: case SQL_CONVERT_LONGVARCHAR: case SQL_CONVERT_NUMERIC: case SQL_CONVERT_REAL:
    case SQL_CONVERT_SMALLINT: case SQL_CONVERT_TIME: case SQL_CONVERT_TIMESTAMP: case SQL_CONVERT_TINYINT:
    case SQL_CONVERT_VARBINARY: case SQL_CONVERT_VARCHAR: case SQL_CONVERT_LONGVARBINARY:
    case SQL_CONVERT_WCHAR: case SQL_CONVERT_WLONGVARCHAR: case SQL_CONVERT_WVARCHAR:
    case SQL_CONVERT_INTERVAL_DAY_TIME: case SQL_CONVERT_INTERVAL_YEAR_MONTH: case SQL_CONVERT_GUID:
        U32(0);
        break;

    /* DDL support masks */
    case SQL_CREATE_SCHEMA: U32(SQL_CS_CREATE_SCHEMA); break;
    case SQL_DROP_SCHEMA: U32(SQL_DS_DROP_SCHEMA | SQL_DS_CASCADE | SQL_DS_RESTRICT); break;
    case SQL_CREATE_TABLE: U32(SQL_CT_CREATE_TABLE | SQL_CT_COLUMN_CONSTRAINT | SQL_CT_TABLE_CONSTRAINT | SQL_CT_COLUMN_DEFAULT); break;
    case SQL_DROP_TABLE: U32(SQL_DT_DROP_TABLE | SQL_DT_CASCADE | SQL_DT_RESTRICT); break;
    case SQL_CREATE_VIEW: U32(SQL_CV_CREATE_VIEW | SQL_CV_CHECK_OPTION | SQL_CV_CASCADED | SQL_CV_LOCAL); break;
    case SQL_DROP_VIEW: U32(SQL_DV_DROP_VIEW | SQL_DV_CASCADE | SQL_DV_RESTRICT); break;
    case SQL_CREATE_ASSERTION: case SQL_CREATE_CHARACTER_SET: case SQL_CREATE_COLLATION: case SQL_CREATE_DOMAIN:
    case SQL_CREATE_TRANSLATION: case SQL_DROP_ASSERTION: case SQL_DROP_CHARACTER_SET: case SQL_DROP_COLLATION:
    case SQL_DROP_DOMAIN: case SQL_DROP_TRANSLATION:
        U32(0);
        break;
    case SQL_SQL92_DATETIME_FUNCTIONS: U32(SQL_SDF_CURRENT_DATE | SQL_SDF_CURRENT_TIME | SQL_SDF_CURRENT_TIMESTAMP); break;
    case SQL_SQL92_FOREIGN_KEY_DELETE_RULE: U32(SQL_SFKD_CASCADE | SQL_SFKD_NO_ACTION | SQL_SFKD_SET_DEFAULT | SQL_SFKD_SET_NULL); break;
    case SQL_SQL92_FOREIGN_KEY_UPDATE_RULE: U32(SQL_SFKU_NO_ACTION); break;
    case SQL_SQL92_GRANT: U32(SQL_SG_DELETE_TABLE | SQL_SG_INSERT_TABLE | SQL_SG_REFERENCES_TABLE | SQL_SG_SELECT_TABLE | SQL_SG_UPDATE_TABLE | SQL_SG_WITH_GRANT_OPTION); break;
    case SQL_SQL92_REVOKE: U32(SQL_SR_DELETE_TABLE | SQL_SR_INSERT_TABLE | SQL_SR_REFERENCES_TABLE | SQL_SR_SELECT_TABLE | SQL_SR_UPDATE_TABLE | SQL_SR_GRANT_OPTION_FOR | SQL_SR_CASCADE | SQL_SR_RESTRICT); break;
    case SQL_SQL92_NUMERIC_VALUE_FUNCTIONS: U32(SQL_SNVF_CHAR_LENGTH | SQL_SNVF_CHARACTER_LENGTH | SQL_SNVF_EXTRACT | SQL_SNVF_OCTET_LENGTH | SQL_SNVF_POSITION); break;
    case SQL_SQL92_PREDICATES: U32(SQL_SP_BETWEEN | SQL_SP_COMPARISON | SQL_SP_EXISTS | SQL_SP_IN | SQL_SP_ISNOTNULL | SQL_SP_ISNULL | SQL_SP_LIKE | SQL_SP_QUANTIFIED_COMPARISON); break;
    case SQL_SQL92_RELATIONAL_JOIN_OPERATORS: U32(SQL_SRJO_CROSS_JOIN | SQL_SRJO_EXCEPT_JOIN | SQL_SRJO_FULL_OUTER_JOIN | SQL_SRJO_INNER_JOIN | SQL_SRJO_INTERSECT_JOIN | SQL_SRJO_LEFT_OUTER_JOIN | SQL_SRJO_RIGHT_OUTER_JOIN | SQL_SRJO_UNION_JOIN); break;
    case SQL_SQL92_ROW_VALUE_CONSTRUCTOR: U32(SQL_SRVC_VALUE_EXPRESSION | SQL_SRVC_NULL | SQL_SRVC_DEFAULT | SQL_SRVC_ROW_SUBQUERY); break;
    case SQL_SQL92_STRING_FUNCTIONS: U32(SQL_SSF_LOWER | SQL_SSF_UPPER | SQL_SSF_SUBSTRING | SQL_SSF_TRIM_BOTH | SQL_SSF_TRIM_LEADING | SQL_SSF_TRIM_TRAILING); break;
    case SQL_SQL92_VALUE_EXPRESSIONS: U32(SQL_SVE_CASE | SQL_SVE_CAST | SQL_SVE_COALESCE | SQL_SVE_NULLIF); break;
    default:
        mapepire_diag_add(&d->h, "HY096", 0, "Information type out of range (%u)", (unsigned)_info_type);
        return mapepire_ret(&d->h, SQL_ERROR);
    }
#undef STR
#undef U16
#undef U32

    if (str) {
        if (mapepire_to_w_bytes_small(str, _info_value, _buffer_length, _string_length) && _info_value) {
            mapepire_diag_add(&d->h, "01004", 0, "String data, right truncated");
            return mapepire_ret(&d->h, SQL_SUCCESS_WITH_INFO);
        }
        return SQL_SUCCESS;
    }
    if (kind == 1) {
        if (_info_value) *(SQLUSMALLINT *)_info_value = (SQLUSMALLINT)u;
        if (_string_length) *_string_length = sizeof(SQLUSMALLINT);
    } else {
        if (_info_value) *(SQLUINTEGER *)_info_value = u;
        if (_string_length) *_string_length = sizeof(SQLUINTEGER);
    }
    return SQL_SUCCESS;
}

/* ---- SQLGetFunctions ----------------------------------------------------------------- */

static const SQLUSMALLINT SUPPORTED[] = {
    SQL_API_SQLALLOCHANDLE, SQL_API_SQLFREEHANDLE, SQL_API_SQLSETENVATTR, SQL_API_SQLGETENVATTR,
    SQL_API_SQLCONNECT, SQL_API_SQLDRIVERCONNECT, SQL_API_SQLDISCONNECT, SQL_API_SQLGETINFO,
    SQL_API_SQLGETFUNCTIONS, SQL_API_SQLSETCONNECTATTR, SQL_API_SQLGETCONNECTATTR, SQL_API_SQLENDTRAN,
    SQL_API_SQLNATIVESQL, SQL_API_SQLPREPARE, SQL_API_SQLEXECUTE, SQL_API_SQLEXECDIRECT, SQL_API_SQLNUMPARAMS,
    SQL_API_SQLBINDPARAMETER, SQL_API_SQLDESCRIBEPARAM, SQL_API_SQLPARAMDATA, SQL_API_SQLPUTDATA,
    SQL_API_SQLNUMRESULTCOLS, SQL_API_SQLDESCRIBECOL, SQL_API_SQLCOLATTRIBUTE, SQL_API_SQLBINDCOL, SQL_API_SQLFETCH,
    SQL_API_SQLFETCHSCROLL, SQL_API_SQLGETDATA, SQL_API_SQLROWCOUNT, SQL_API_SQLMORERESULTS, SQL_API_SQLCLOSECURSOR,
    SQL_API_SQLCANCEL, SQL_API_SQLFREESTMT, SQL_API_SQLGETSTMTATTR, SQL_API_SQLSETSTMTATTR, SQL_API_SQLGETCURSORNAME,
    SQL_API_SQLSETCURSORNAME, SQL_API_SQLGETDIAGREC, SQL_API_SQLGETDIAGFIELD, SQL_API_SQLTABLES, SQL_API_SQLCOLUMNS,
    SQL_API_SQLPRIMARYKEYS, SQL_API_SQLFOREIGNKEYS, SQL_API_SQLSTATISTICS, SQL_API_SQLSPECIALCOLUMNS,
    SQL_API_SQLPROCEDURES, SQL_API_SQLPROCEDURECOLUMNS, SQL_API_SQLGETTYPEINFO, SQL_API_SQLTABLEPRIVILEGES,
    SQL_API_SQLCOLUMNPRIVILEGES,
};

static int supported(SQLUSMALLINT _id)
{
    for (size_t i = 0; i < sizeof SUPPORTED / sizeof SUPPORTED[0]; i++)
        if (SUPPORTED[i] == _id) return 1;
    return 0;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetFunctions(SQLHDBC _connection_handle, SQLUSMALLINT _function_id, SQLUSMALLINT *_supported)
{
    if (!MAPEPIRE_VALID(_connection_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
    mapepire_dbc *d = (mapepire_dbc *)_connection_handle;
    mapepire_diag_clear(&d->h);
    if (!_supported) return SQL_SUCCESS;
    if (_function_id == SQL_API_ODBC3_ALL_FUNCTIONS) {
        memset(_supported, 0, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * sizeof(SQLUSMALLINT));
        for (size_t i = 0; i < sizeof SUPPORTED / sizeof SUPPORTED[0]; i++) {
            SQLUSMALLINT id = SUPPORTED[i];
            _supported[id >> 4] |= (SQLUSMALLINT)(1 << (id & 0x000F));
        }
        return SQL_SUCCESS;
    }
    if (_function_id == SQL_API_ALL_FUNCTIONS) {
        memset(_supported, 0, 100 * sizeof(SQLUSMALLINT));
        for (size_t i = 0; i < sizeof SUPPORTED / sizeof SUPPORTED[0]; i++)
            if (SUPPORTED[i] < 100) _supported[SUPPORTED[i]] = SQL_TRUE;
        return SQL_SUCCESS;
    }
    *_supported = supported(_function_id) ? SQL_TRUE : SQL_FALSE;
    return SQL_SUCCESS;
}

/* ---- SQLGetTypeInfoW ---------------------------------------------------------------- */

typedef struct {
    const char *name;
    SQLSMALLINT type;
    long        size;
    const char *prefix, *suffix, *params;
    SQLSMALLINT case_sensitive, searchable, is_unsigned, min_scale, max_scale, radix;
} typeinfo;

/* Sorted by DATA_TYPE as ODBC requires. -1 means NULL for the numeric fields. */
static const typeinfo TYPES[] = {
    {"DBCLOB", SQL_WLONGVARCHAR, 1073741822L, "'", "'", "length", 1, SQL_PRED_CHAR, -1, -1, -1, -1},
    {"VARGRAPHIC", SQL_WVARCHAR, 16370, "'", "'", "length", 1, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"NVARCHAR", SQL_WVARCHAR, 16370, "'", "'", "length", 1, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"GRAPHIC", SQL_WCHAR, 16352, "'", "'", "length", 1, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"NCHAR", SQL_WCHAR, 16352, "'", "'", "length", 1, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"BOOLEAN", SQL_BIT, 1, NULL, NULL, NULL, 0, SQL_SEARCHABLE, 1, -1, -1, -1},
    {"BIGINT", SQL_BIGINT, 19, NULL, NULL, NULL, 0, SQL_PRED_BASIC, 0, 0, 0, 10},
    {"BLOB", SQL_LONGVARBINARY, 2147483647L, "BLOB(X'", "')", "length", 0, SQL_PRED_NONE, -1, -1, -1, -1},
    {"VARBINARY", SQL_VARBINARY, 32740, "BX'", "'", "length", 0, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"BINARY", SQL_BINARY, 32766, "BX'", "'", "length", 0, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"CLOB", SQL_LONGVARCHAR, 2147483647L, "'", "'", "length", 1, SQL_PRED_CHAR, -1, -1, -1, -1},
    {"CHAR", SQL_CHAR, 32765, "'", "'", "length", 1, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"NUMERIC", SQL_NUMERIC, 63, NULL, NULL, "precision,scale", 0, SQL_PRED_BASIC, 0, 0, 63, 10},
    {"DECIMAL", SQL_DECIMAL, 63, NULL, NULL, "precision,scale", 0, SQL_PRED_BASIC, 0, 0, 63, 10},
    {"INTEGER", SQL_INTEGER, 10, NULL, NULL, NULL, 0, SQL_PRED_BASIC, 0, 0, 0, 10},
    {"SMALLINT", SQL_SMALLINT, 5, NULL, NULL, NULL, 0, SQL_PRED_BASIC, 0, 0, 0, 10},
    {"REAL", SQL_REAL, 24, NULL, NULL, NULL, 0, SQL_PRED_BASIC, 0, -1, -1, 2},
    {"DOUBLE", SQL_DOUBLE, 53, NULL, NULL, NULL, 0, SQL_PRED_BASIC, 0, -1, -1, 2},
    {"VARCHAR", SQL_VARCHAR, 32739, "'", "'", "length", 1, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"DATE", SQL_TYPE_DATE, 10, "'", "'", NULL, 0, SQL_SEARCHABLE, -1, -1, -1, -1},
    {"TIME", SQL_TYPE_TIME, 8, "'", "'", NULL, 0, SQL_SEARCHABLE, -1, 0, 0, -1},
    {"TIMESTAMP", SQL_TYPE_TIMESTAMP, 32, "'", "'", "precision", 0, SQL_SEARCHABLE, -1, 0, 12, -1},
};

static mpjson *nint(long _v) { return _v < 0 ? mpjson_new(MPJSON_NULL) : mpjson_new_int(_v); }
static mpjson *nstr(const char *_s) { return _s ? mpjson_new_string(_s) : mpjson_new(MPJSON_NULL); }

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetTypeInfoW(SQLHSTMT _statement_handle, SQLSMALLINT _data_type)
{
    if (!MAPEPIRE_VALID(_statement_handle, MAPEPIRE_H_STMT)) return SQL_INVALID_HANDLE;
    mapepire_stmt *s = (mapepire_stmt *)_statement_handle;
    mapepire_diag_clear(&s->h);
    if (s->has_result) {
        mapepire_diag_add(&s->h, "24000", 0, "Invalid cursor state");
        return mapepire_ret(&s->h, SQL_ERROR);
    }
    if (_data_type == SQL_DATE) _data_type = SQL_TYPE_DATE;
    if (_data_type == SQL_TIME) _data_type = SQL_TYPE_TIME;
    if (_data_type == SQL_TIMESTAMP) _data_type = SQL_TYPE_TIMESTAMP;
    static const char *const names[] = {"TYPE_NAME", "DATA_TYPE", "COLUMN_SIZE", "LITERAL_PREFIX", "LITERAL_SUFFIX",
                                        "CREATE_PARAMS", "NULLABLE", "CASE_SENSITIVE", "SEARCHABLE",
                                        "UNSIGNED_ATTRIBUTE", "FIXED_PREC_SCALE", "AUTO_UNIQUE_VALUE",
                                        "LOCAL_TYPE_NAME", "MINIMUM_SCALE", "MAXIMUM_SCALE", "SQL_DATA_TYPE",
                                        "SQL_DATETIME_SUB", "NUM_PREC_RADIX", "INTERVAL_PRECISION"};
    static const SQLSMALLINT types[] = {SQL_VARCHAR, SQL_SMALLINT, SQL_INTEGER, SQL_VARCHAR, SQL_VARCHAR,
                                        SQL_VARCHAR, SQL_SMALLINT, SQL_SMALLINT, SQL_SMALLINT, SQL_SMALLINT,
                                        SQL_SMALLINT, SQL_SMALLINT, SQL_VARCHAR, SQL_SMALLINT, SQL_SMALLINT,
                                        SQL_SMALLINT, SQL_SMALLINT, SQL_INTEGER, SQL_SMALLINT};
    mpjson *rows = mpjson_new(MPJSON_ARRAY);
    for (size_t i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) {
        const typeinfo *t = &TYPES[i];
        if (_data_type != SQL_ALL_TYPES && _data_type != t->type) continue;
        mpjson *r = mpjson_new(MPJSON_ARRAY);
        mpjson_push(r, mpjson_new_string(t->name));
        mpjson_push(r, mpjson_new_int(mapepire_odbc2_type(s->dbc, t->type)));
        mpjson_push(r, mpjson_new_int(t->size));
        mpjson_push(r, nstr(t->prefix));
        mpjson_push(r, nstr(t->suffix));
        mpjson_push(r, nstr(t->params));
        mpjson_push(r, mpjson_new_int(SQL_NULLABLE));
        mpjson_push(r, mpjson_new_int(t->case_sensitive));
        mpjson_push(r, mpjson_new_int(t->searchable));
        mpjson_push(r, nint(t->is_unsigned));
        mpjson_push(r, mpjson_new_int(SQL_FALSE));
        mpjson_push(r, t->type == SQL_INTEGER || t->type == SQL_BIGINT || t->type == SQL_SMALLINT ? mpjson_new_int(SQL_FALSE)
                                                                                             : mpjson_new(MPJSON_NULL));
        mpjson_push(r, mpjson_new_string(t->name));
        mpjson_push(r, nint(t->min_scale));
        mpjson_push(r, nint(t->max_scale));
        mpjson_push(r, mpjson_new_int(mapepire_type_verbose(t->type)));
        mpjson_push(r, mapepire_type_datetime_sub(t->type) ? mpjson_new_int(mapepire_type_datetime_sub(t->type)) : mpjson_new(MPJSON_NULL));
        mpjson_push(r, nint(t->radix));
        mpjson_push(r, mpjson_new(MPJSON_NULL));
        mpjson_push(rows, r);
    }
    mapepire_stmt_local_result(s, names, types, 19, rows);
    return SQL_SUCCESS;
}

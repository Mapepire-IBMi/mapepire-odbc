/*
 * Handle allocation and environment attributes.
 */
#include "drv.h"

#include <stdlib.h>
#include <string.h>

mapepire_stmt *mapepire_stmt_new(mapepire_dbc *_d);
void     mapepire_stmt_destroy(mapepire_stmt *_s);
void     mapepire_dbc_destroy(mapepire_dbc *_d);

static SQLRETURN alloc_env(SQLHANDLE *_out)
{
    mapepire_env *e = (mapepire_env *)calloc(1, sizeof *e);
    if (!e) return SQL_ERROR;
    e->h.magic = MAPEPIRE_H_ENV;
    e->odbc_version = 0;
    e->connection_pooling = SQL_CP_OFF;
    e->cp_match = SQL_CP_STRICT_MATCH;
    mapepire_mutex_init(&e->lock);
    *_out = e;
    return SQL_SUCCESS;
}

static SQLRETURN alloc_dbc(mapepire_env *_e, SQLHANDLE *_out)
{
    mapepire_diag_clear(&_e->h);
    if (!_e->odbc_version) {
        mapepire_diag_add(&_e->h, "HY010", 0, "SQL_ATTR_ODBC_VERSION must be set before allocating a connection");
        return mapepire_ret(&_e->h, SQL_ERROR);
    }
    mapepire_dbc *d = (mapepire_dbc *)calloc(1, sizeof *d);
    if (!d) {
        mapepire_diag_add(&_e->h, "HY001", 0, "Out of memory");
        return mapepire_ret(&_e->h, SQL_ERROR);
    }
    d->h.magic = MAPEPIRE_H_DBC;
    d->env = _e;
    d->autocommit = SQL_AUTOCOMMIT_ON;
    d->access_mode = SQL_MODE_READ_WRITE;
    d->login_timeout = 0;
    d->block_size = MAPEPIRE_DEFAULT_BLOCK;
    mapepire_mutex_init(&d->lock);
    mapepire_mutex_lock(&_e->lock);
    d->next = _e->dbcs;
    _e->dbcs = d;
    mapepire_mutex_unlock(&_e->lock);
    *_out = d;
    return SQL_SUCCESS;
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT _handle_type, SQLHANDLE _input_handle, SQLHANDLE *_output_handle)
{
    if (!_output_handle) return SQL_ERROR;
    *_output_handle = SQL_NULL_HANDLE;
    switch (_handle_type) {
    case SQL_HANDLE_ENV:
        return alloc_env(_output_handle);
    case SQL_HANDLE_DBC:
        if (!MAPEPIRE_VALID(_input_handle, MAPEPIRE_H_ENV)) return SQL_INVALID_HANDLE;
        return alloc_dbc((mapepire_env *)_input_handle, _output_handle);
    case SQL_HANDLE_STMT: {
        if (!MAPEPIRE_VALID(_input_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
        mapepire_dbc *d = (mapepire_dbc *)_input_handle;
        mapepire_diag_clear(&d->h);
        if (!mapepire_dbc_connected(d)) {
            mapepire_diag_add(&d->h, "08003", 0, "Connection not open");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        mapepire_stmt *s = mapepire_stmt_new(d);
        if (!s) {
            mapepire_diag_add(&d->h, "HY001", 0, "Out of memory");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        *_output_handle = s;
        return SQL_SUCCESS;
    }
    case SQL_HANDLE_DESC: {
        if (!MAPEPIRE_VALID(_input_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
        mapepire_dbc *d = (mapepire_dbc *)_input_handle;
        mapepire_diag_clear(&d->h);
        mapepire_diag_add(&d->h, "HYC00", 0, "Explicitly allocated descriptors are not supported");
        return mapepire_ret(&d->h, SQL_ERROR);
    }
    default:
        return SQL_ERROR;
    }
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT _handle_type, SQLHANDLE _handle)
{
    switch (_handle_type) {
    case SQL_HANDLE_ENV: {
        if (!MAPEPIRE_VALID(_handle, MAPEPIRE_H_ENV)) return SQL_INVALID_HANDLE;
        mapepire_env *e = (mapepire_env *)_handle;
        if (e->dbcs) {
            mapepire_diag_clear(&e->h);
            mapepire_diag_add(&e->h, "HY010", 0, "Connections are still allocated on this environment");
            return mapepire_ret(&e->h, SQL_ERROR);
        }
        mapepire_diag_free(&e->h.diag);
        mapepire_mutex_destroy(&e->lock);
        e->h.magic = 0;
        free(e);
        return SQL_SUCCESS;
    }
    case SQL_HANDLE_DBC: {
        if (!MAPEPIRE_VALID(_handle, MAPEPIRE_H_DBC)) return SQL_INVALID_HANDLE;
        mapepire_dbc *d = (mapepire_dbc *)_handle;
        if (mapepire_dbc_connected(d)) {
            mapepire_diag_clear(&d->h);
            mapepire_diag_add(&d->h, "HY010", 0, "The connection is still open; call SQLDisconnect first");
            return mapepire_ret(&d->h, SQL_ERROR);
        }
        mapepire_env *e = d->env;
        mapepire_mutex_lock(&e->lock);
        for (mapepire_dbc **pp = &e->dbcs; *pp; pp = &(*pp)->next) {
            if (*pp == d) { *pp = d->next; break; }
        }
        mapepire_mutex_unlock(&e->lock);
        mapepire_dbc_destroy(d);
        return SQL_SUCCESS;
    }
    case SQL_HANDLE_STMT:
        if (!MAPEPIRE_VALID(_handle, MAPEPIRE_H_STMT)) return SQL_INVALID_HANDLE;
        mapepire_stmt_destroy((mapepire_stmt *)_handle);
        return SQL_SUCCESS;
    case SQL_HANDLE_DESC:
        if (!MAPEPIRE_VALID(_handle, MAPEPIRE_H_DESC)) return SQL_INVALID_HANDLE;
        /* implicit descriptors are freed with their statement */
        mapepire_diag_clear((mapepire_handle *)_handle);
        mapepire_diag_add((mapepire_handle *)_handle, "HY017", 0, "Invalid use of an automatically allocated descriptor handle");
        return SQL_ERROR;
    default:
        return SQL_ERROR;
    }
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV _environment_handle, SQLINTEGER _attribute, SQLPOINTER _value,
                                          SQLINTEGER _string_length)
{
    (void)_string_length;
    if (!MAPEPIRE_VALID(_environment_handle, MAPEPIRE_H_ENV)) return SQL_INVALID_HANDLE;
    mapepire_env *e = (mapepire_env *)_environment_handle;
    mapepire_diag_clear(&e->h);
    SQLINTEGER v = (SQLINTEGER)(SQLLEN)_value;
    switch (_attribute) {
    case SQL_ATTR_ODBC_VERSION:
        if (v != SQL_OV_ODBC2 && v != SQL_OV_ODBC3
#ifdef SQL_OV_ODBC3_80
            && v != SQL_OV_ODBC3_80
#endif
        ) {
            mapepire_diag_add(&e->h, "HY024", 0, "Invalid attribute value");
            return mapepire_ret(&e->h, SQL_ERROR);
        }
        e->odbc_version = v;
        return SQL_SUCCESS;
    case SQL_ATTR_CONNECTION_POOLING:
        e->connection_pooling = v;
        return SQL_SUCCESS;
    case SQL_ATTR_CP_MATCH:
        e->cp_match = v;
        return SQL_SUCCESS;
    case SQL_ATTR_OUTPUT_NTS:
        if (v == SQL_TRUE) return SQL_SUCCESS;
        mapepire_diag_add(&e->h, "HYC00", 0, "Only SQL_TRUE is supported for SQL_ATTR_OUTPUT_NTS");
        return mapepire_ret(&e->h, SQL_ERROR);
    default:
        mapepire_diag_add(&e->h, "HY092", 0, "Invalid attribute/option identifier");
        return mapepire_ret(&e->h, SQL_ERROR);
    }
}

MAPEPIRE_EXPORT SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV _environment_handle, SQLINTEGER _attribute, SQLPOINTER _value,
                                          SQLINTEGER _buffer_length, SQLINTEGER *_string_length)
{
    (void)_buffer_length;
    if (!MAPEPIRE_VALID(_environment_handle, MAPEPIRE_H_ENV)) return SQL_INVALID_HANDLE;
    mapepire_env *e = (mapepire_env *)_environment_handle;
    mapepire_diag_clear(&e->h);
    SQLINTEGER v;
    switch (_attribute) {
    case SQL_ATTR_ODBC_VERSION: v = e->odbc_version; break;
    case SQL_ATTR_CONNECTION_POOLING: v = e->connection_pooling; break;
    case SQL_ATTR_CP_MATCH: v = e->cp_match; break;
    case SQL_ATTR_OUTPUT_NTS: v = SQL_TRUE; break;
    default:
        mapepire_diag_add(&e->h, "HY092", 0, "Invalid attribute/option identifier");
        return mapepire_ret(&e->h, SQL_ERROR);
    }
    if (_value) *(SQLINTEGER *)_value = v;
    if (_string_length) *_string_length = sizeof(SQLINTEGER);
    return SQL_SUCCESS;
}

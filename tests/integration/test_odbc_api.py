"""
Low-level tests that call the ODBC API directly (through the platform Driver
Manager) with ctypes, covering behaviour pyodbc never exercises: block
cursors, chunked SQLGetData, every C type conversion, output parameters,
data-at-execution, parameter arrays, and diagnostics.
"""
import ctypes
import ctypes.util
import struct
import sys

import pytest

if sys.platform == "win32":
    odbc = ctypes.windll.odbc32
else:  # pragma: no cover - exercised on Linux CI
    odbc = ctypes.CDLL(ctypes.util.find_library("odbc") or "libodbc.so.2")

SQLLEN = ctypes.c_ssize_t
SQLULEN = ctypes.c_size_t
H = ctypes.c_void_p
_P = ctypes.c_void_p
_S, _US, _I = ctypes.c_int16, ctypes.c_uint16, ctypes.c_int32

# Exact prototypes matter on 64-bit: SQLLEN/SQLULEN are 8 bytes and SQLRETURN is a short.
for _name, _args in {
    "SQLAllocHandle": [_S, H, _P], "SQLFreeHandle": [_S, H], "SQLSetEnvAttr": [H, _I, _P, _I],
    "SQLDriverConnectW": [H, _P, _P, _S, _P, _S, _P, _US], "SQLDriverConnect": [H, _P, _P, _S, _P, _S, _P, _US],
    "SQLDisconnect": [H], "SQLGetFunctions": [H, _US, _P], "SQLNativeSqlW": [H, _P, _I, _P, _I, _P],
    "SQLExecDirectW": [H, _P, _I], "SQLPrepareW": [H, _P, _I], "SQLExecute": [H], "SQLFetch": [H],
    "SQLBindCol": [H, _US, _S, _P, SQLLEN, _P], "SQLGetData": [H, _US, _S, _P, SQLLEN, _P],
    "SQLBindParameter": [H, _US, _S, _S, _S, SQLULEN, _S, _P, SQLLEN, _P],
    "SQLSetStmtAttrW": [H, _I, _P, _I], "SQLPutData": [H, _P, SQLLEN], "SQLParamData": [H, _P],
    "SQLRowCount": [H, _P], "SQLMoreResults": [H], "SQLCloseCursor": [H], "SQLFreeStmt": [H, _US],
    "SQLNumResultCols": [H, _P], "SQLNumParams": [H, _P], "SQLDescribeColW": [H, _US, _P, _S, _P, _P, _P, _P, _P],
    "SQLDescribeParam": [H, _US, _P, _P, _P, _P], "SQLGetDiagRecW": [_S, H, _S, _P, _P, _P, _S, _P],
    "SQLGetDiagFieldW": [_S, H, _S, _S, _P, _S, _P],
}.items():
    _f = getattr(odbc, _name)
    _f.argtypes = _args
    _f.restype = ctypes.c_int16

# constants
SQL_SUCCESS, SQL_SUCCESS_WITH_INFO, SQL_NO_DATA, SQL_ERROR, SQL_NEED_DATA = 0, 1, 100, -1, 99
SQL_HANDLE_ENV, SQL_HANDLE_DBC, SQL_HANDLE_STMT = 1, 2, 3
SQL_ATTR_ODBC_VERSION, SQL_OV_ODBC3 = 200, 3
SQL_NTS, SQL_NULL_DATA, SQL_DATA_AT_EXEC = -3, -1, -2
SQL_C_CHAR, SQL_C_WCHAR, SQL_C_SLONG, SQL_C_SBIGINT, SQL_C_DOUBLE, SQL_C_BINARY = 1, -8, -16, -25, 8, -2
SQL_C_BIT, SQL_C_NUMERIC, SQL_C_TYPE_DATE, SQL_C_TYPE_TIMESTAMP, SQL_C_SSHORT = -7, 2, 91, 93, -15
SQL_INTEGER, SQL_VARCHAR, SQL_WVARCHAR, SQL_DECIMAL, SQL_LONGVARCHAR = 4, 12, -9, 3, -1
SQL_PARAM_INPUT, SQL_PARAM_OUTPUT = 1, 4
SQL_ATTR_ROW_ARRAY_SIZE, SQL_ATTR_ROW_BIND_TYPE, SQL_ATTR_ROW_STATUS_PTR, SQL_ATTR_ROWS_FETCHED_PTR = 27, 5, 25, 26
SQL_ATTR_MAX_ROWS, SQL_ATTR_CURSOR_TYPE, SQL_CURSOR_STATIC = 1, 6, 3
SQL_ATTR_PARAMSET_SIZE, SQL_ATTR_PARAM_STATUS_PTR, SQL_ATTR_PARAMS_PROCESSED_PTR = 22, 20, 21
SQL_ATTR_PARAM_OPERATION_PTR, SQL_PARAM_IGNORE, SQL_PARAM_SUCCESS, SQL_PARAM_UNUSED = 19, 1, 0, 7
SQL_ROW_SUCCESS, SQL_ROW_NOROW = 0, 3
SQL_DIAG_NUMBER, SQL_DIAG_SQLSTATE, SQL_DIAG_CLASS_ORIGIN = 2, 4, 8
SQL_DRIVER_NOPROMPT, SQL_CLOSE = 0, 0
SQL_API_SQLDESCRIBEPARAM, SQL_API_SQLBROWSECONNECT, SQL_API_ODBC3_ALL_FUNCTIONS = 58, 55, 999


def wstr(s):
    return ctypes.create_string_buffer((s + "\0").encode("utf-16-le"))


def check(rc, h=None, htype=SQL_HANDLE_STMT, ok=(SQL_SUCCESS, SQL_SUCCESS_WITH_INFO)):
    if rc not in ok:
        raise AssertionError(f"rc={rc} diag={diag(htype, h) if h else None}")
    return rc


def diag(htype, h):
    out = []
    i = 1
    while True:
        state = ctypes.create_string_buffer(12)
        native = ctypes.c_int32()
        msg = ctypes.create_string_buffer(2048)
        ln = ctypes.c_int16()
        rc = odbc.SQLGetDiagRecW(htype, h, i, state, ctypes.byref(native), msg,
                                 1024, ctypes.byref(ln))
        if rc not in (SQL_SUCCESS, SQL_SUCCESS_WITH_INFO):
            return out
        out.append((state.raw[:10].decode("utf-16-le"), native.value, msg.raw[: ln.value * 2].decode("utf-16-le")))
        i += 1


def states(h):
    return [d[0] for d in diag(SQL_HANDLE_STMT, h)]


class Conn:
    def __init__(self, connstr, ansi=False):
        self.env, self.dbc = H(), H()
        check(odbc.SQLAllocHandle(SQL_HANDLE_ENV, None, ctypes.byref(self.env)))
        check(odbc.SQLSetEnvAttr(self.env, SQL_ATTR_ODBC_VERSION, H(SQL_OV_ODBC3), 0))
        check(odbc.SQLAllocHandle(SQL_HANDLE_DBC, self.env, ctypes.byref(self.dbc)))
        if ansi:
            rc = odbc.SQLDriverConnect(self.dbc, None, connstr.encode("mbcs" if sys.platform == "win32" else "utf-8"),
                                       SQL_NTS, None, 0, None, SQL_DRIVER_NOPROMPT)
        else:
            rc = odbc.SQLDriverConnectW(self.dbc, None, wstr(connstr), SQL_NTS, None, 0, None, SQL_DRIVER_NOPROMPT)
        check(rc, self.dbc, SQL_HANDLE_DBC)

    def stmt(self):
        s = H()
        check(odbc.SQLAllocHandle(SQL_HANDLE_STMT, self.dbc, ctypes.byref(s)))
        return s

    def close(self):
        odbc.SQLDisconnect(self.dbc)
        odbc.SQLFreeHandle(SQL_HANDLE_DBC, self.dbc)
        odbc.SQLFreeHandle(SQL_HANDLE_ENV, self.env)


def exec_direct(s, sql, ok=(SQL_SUCCESS, SQL_SUCCESS_WITH_INFO)):
    return check(odbc.SQLExecDirectW(s, wstr(sql), SQL_NTS), s, ok=ok)


def free(s):
    odbc.SQLFreeHandle(SQL_HANDLE_STMT, s)


@pytest.fixture()
def conn(connstr):
    c = Conn(connstr(blocksize=4))
    yield c
    c.close()


def get_data(s, col, ctype, size):
    buf = ctypes.create_string_buffer(size)
    ind = SQLLEN()
    rc = odbc.SQLGetData(s, ctypes.c_uint16(col), ctypes.c_int16(ctype), buf, SQLLEN(size), ctypes.byref(ind))
    return rc, buf, ind.value


# ---- block cursors ------------------------------------------------------------------


def test_column_wise_array_fetch_across_blocks(conn):
    s = conn.stmt()
    n = 5
    dept = ctypes.create_string_buffer(8 * n)
    dept_ind = (SQLLEN * n)()
    loc = ctypes.create_string_buffer(20 * n)
    loc_ind = (SQLLEN * n)()
    status = (ctypes.c_uint16 * n)()
    fetched = SQLULEN()
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_ROW_ARRAY_SIZE, H(n), 0), s)
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_ROW_STATUS_PTR, status, 0), s)
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_ROWS_FETCHED_PTR, ctypes.byref(fetched), 0), s)
    exec_direct(s, "SELECT * FROM SAMPLE.DEPARTMENT")
    check(odbc.SQLBindCol(s, 1, SQL_C_CHAR, dept, SQLLEN(8), dept_ind), s)
    check(odbc.SQLBindCol(s, 5, SQL_C_CHAR, loc, SQLLEN(20), loc_ind), s)
    seen, counts = [], []
    while odbc.SQLFetch(s) == SQL_SUCCESS:
        counts.append(fetched.value)
        for i in range(fetched.value):
            assert status[i] == SQL_ROW_SUCCESS
            seen.append(dept.raw[i * 8: i * 8 + dept_ind[i]].decode())
            assert loc_ind[i] == SQL_NULL_DATA
        for i in range(fetched.value, n):
            assert status[i] == SQL_ROW_NOROW
    assert counts == [5, 5, 4]
    assert seen[:3] == ["A00", "B01", "C01"] and len(seen) == 14 and len(set(seen)) == 14
    free(s)


def test_row_wise_array_fetch(conn):
    class Row(ctypes.Structure):
        _fields_ = [("deptno", ctypes.c_char * 4), ("deptno_ind", SQLLEN),
                    ("name", ctypes.c_uint16 * 40), ("name_ind", SQLLEN)]

    s = conn.stmt()
    rows = (Row * 3)()
    fetched = SQLULEN()
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_ROW_ARRAY_SIZE, H(3), 0), s)
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_ROW_BIND_TYPE, H(ctypes.sizeof(Row)), 0), s)
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_ROWS_FETCHED_PTR, ctypes.byref(fetched), 0), s)
    exec_direct(s, "SELECT * FROM SAMPLE.DEPARTMENT")
    check(odbc.SQLBindCol(s, 1, SQL_C_CHAR, ctypes.byref(rows[0], Row.deptno.offset), SQLLEN(4),
                          ctypes.byref(rows[0], Row.deptno_ind.offset)), s)
    check(odbc.SQLBindCol(s, 2, SQL_C_WCHAR, ctypes.byref(rows[0], Row.name.offset), SQLLEN(80),
                          ctypes.byref(rows[0], Row.name_ind.offset)), s)
    check(odbc.SQLFetch(s), s)
    assert fetched.value == 3
    assert [r.deptno.decode() for r in rows] == ["A00", "B01", "C01"]
    name0 = bytes(rows[0].name)[: rows[0].name_ind].decode("utf-16-le")
    assert name0 == "SPIFFY COMPUTER SERVICE DIV."
    free(s)


def test_max_rows(conn):
    s = conn.stmt()
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_MAX_ROWS, H(3), 0), s)
    exec_direct(s, "SELECT * FROM SAMPLE.DEPARTMENT")
    n = 0
    while odbc.SQLFetch(s) == SQL_SUCCESS:
        n += 1
    assert n == 3
    free(s)


# ---- SQLGetData ------------------------------------------------------------------------


def test_chunked_getdata_wchar_and_char(conn):
    s = conn.stmt()
    exec_direct(s, "SELECT * FROM SAMPLE.DEPARTMENT")
    check(odbc.SQLFetch(s), s)
    want = "SPIFFY COMPUTER SERVICE DIV."
    got = ""
    remaining = []
    while True:
        rc, buf, ind = get_data(s, 2, SQL_C_WCHAR, 10)  # room for 4 characters + NUL
        if rc == SQL_NO_DATA:
            break
        assert rc in (SQL_SUCCESS, SQL_SUCCESS_WITH_INFO)
        if rc == SQL_SUCCESS_WITH_INFO:
            assert states(s) == ["01004"]
        remaining.append(ind)
        got += buf.raw[: min(ind * 1, 8)].decode("utf-16-le") if ind > 0 else ""
    assert got == want
    assert remaining[0] == len(want) * 2 and remaining == sorted(remaining, reverse=True)
    # SQL_C_CHAR chunks of 4 bytes + NUL on the same row, different column
    parts = []
    while True:
        rc, buf, ind = get_data(s, 1, SQL_C_CHAR, 2)
        if rc == SQL_NO_DATA:
            break
        parts.append(buf.value.decode())
    assert "".join(parts) == "A00"
    # NULL column
    rc, buf, ind = get_data(s, 5, SQL_C_CHAR, 10)
    assert rc == SQL_SUCCESS and ind == SQL_NULL_DATA
    assert get_data(s, 5, SQL_C_CHAR, 10)[0] == SQL_NO_DATA
    free(s)


def test_conversions(conn):
    s = conn.stmt()
    exec_direct(s, "SELECT * FROM ODBCTEST.TYPED")
    check(odbc.SQLFetch(s), s)
    # columns: I SI BI DBL C VC D T TS BO BIN
    rc, buf, ind = get_data(s, 1, SQL_C_DOUBLE, 8)
    assert rc == SQL_SUCCESS and struct.unpack("d", buf.raw)[0] == 42.0
    rc, buf, _ = get_data(s, 4, SQL_C_SLONG, 4)  # 2.5 -> 2 with fractional truncation
    assert rc == SQL_SUCCESS_WITH_INFO and states(s) == ["01S07"] and struct.unpack("i", buf.raw)[0] == 2
    rc, buf, _ = get_data(s, 3, SQL_C_SLONG, 4)  # 9007199254740993 does not fit
    assert rc == SQL_ERROR and states(s) == ["22003"]
    rc, buf, _ = get_data(s, 3, SQL_C_SBIGINT, 8)
    assert rc == SQL_SUCCESS and struct.unpack("q", buf.raw)[0] == 9007199254740993
    rc, buf, _ = get_data(s, 6, SQL_C_SLONG, 4)  # text is not a number
    assert rc == SQL_ERROR and states(s) == ["22018"]
    rc, buf, _ = get_data(s, 9, SQL_C_TYPE_DATE, 6)  # timestamp -> date drops the time
    assert rc == SQL_SUCCESS_WITH_INFO and states(s) == ["01S07"]
    assert struct.unpack("hHH", buf.raw[:6]) == (2024, 2, 29)
    rc, buf, _ = get_data(s, 7, SQL_C_TYPE_TIMESTAMP, 16)  # date -> timestamp
    assert rc == SQL_SUCCESS and struct.unpack("hHHHHHI", buf.raw[:16]) == (2024, 2, 29, 0, 0, 0, 0)
    rc, buf, _ = get_data(s, 9, SQL_C_TYPE_TIMESTAMP, 16)
    assert struct.unpack("hHHHHHI", buf.raw[:16]) == (2024, 2, 29, 13, 45, 30, 123456000)
    rc, buf, _ = get_data(s, 10, SQL_C_CHAR, 4)
    assert rc == SQL_SUCCESS and buf.value == b"1"
    rc, buf, ind = get_data(s, 11, SQL_C_BINARY, 16)
    assert rc == SQL_SUCCESS and buf.raw[:ind] == bytes.fromhex("DEADBEEF")
    rc, buf, ind = get_data(s, 11, SQL_C_CHAR, 16)  # binary -> char is hex
    assert buf.value == b"DEADBEEF"
    rc, buf, _ = get_data(s, 9, SQL_C_SLONG, 4)
    assert rc == SQL_ERROR and states(s) == ["07006"]
    free(s)


def test_numeric_struct(conn):
    s = conn.stmt()
    exec_direct(s, "SELECT * FROM ODBCTEST.PRECISE")
    check(odbc.SQLFetch(s), s)
    rc, buf, _ = get_data(s, 3, SQL_C_NUMERIC, 19)  # NUMERIC(5,2) 123.45
    assert rc == SQL_SUCCESS
    precision, scale, sign = buf.raw[0], buf.raw[1], buf.raw[2]
    assert (precision, scale, sign) == (5, 2, 1)
    assert int.from_bytes(buf.raw[3:19], "little") == 12345
    rc, buf, _ = get_data(s, 1, SQL_C_CHAR, 64)
    assert buf.value == b"12345678901234567890.123456789"
    rc, buf, _ = get_data(s, 2, SQL_C_CHAR, 64)
    assert buf.value == b"0.0000001"
    free(s)


# ---- describe before execute ---------------------------------------------------------


def test_describe_before_execute(conn):
    s = conn.stmt()
    check(odbc.SQLPrepareW(s, wstr("SELECT * FROM SAMPLE.DEPARTMENT WHERE ADMRDEPT = ?"), SQL_NTS), s)
    ncols = ctypes.c_int16()
    check(odbc.SQLNumResultCols(s, ctypes.byref(ncols)), s)
    assert ncols.value == 5
    name = ctypes.create_string_buffer(64)
    nlen, dtype, digits, nullable = ctypes.c_int16(), ctypes.c_int16(), ctypes.c_int16(), ctypes.c_int16()
    size = SQLULEN()
    check(odbc.SQLDescribeColW(s, 2, name, 32, ctypes.byref(nlen), ctypes.byref(dtype), ctypes.byref(size),
                               ctypes.byref(digits), ctypes.byref(nullable)), s)
    assert name.raw[: nlen.value * 2].decode("utf-16-le") == "DEPTNAME"
    assert dtype.value == SQL_VARCHAR and size.value == 36 and nullable.value == 0
    nparams = ctypes.c_int16()
    check(odbc.SQLNumParams(s, ctypes.byref(nparams)), s)
    assert nparams.value == 1
    check(odbc.SQLDescribeParam(s, 1, ctypes.byref(dtype), ctypes.byref(size), ctypes.byref(digits),
                                ctypes.byref(nullable)), s)
    assert dtype.value == SQL_VARCHAR  # the mock describes every marker as VARCHAR(255)
    # now execute the prepared statement
    val = ctypes.create_string_buffer(b"E01")
    ind = SQLLEN(SQL_NTS)
    check(odbc.SQLBindParameter(s, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, SQLULEN(3), 0, val, SQLLEN(4),
                                ctypes.byref(ind)), s)
    check(odbc.SQLExecute(s), s)
    n = 0
    while odbc.SQLFetch(s) == SQL_SUCCESS:
        n += 1
    assert n == 7
    # re-execute with another value
    odbc.SQLFreeStmt(s, SQL_CLOSE)
    ctypes.memmove(val, b"A00\0", 4)
    check(odbc.SQLExecute(s), s)
    n = 0
    while odbc.SQLFetch(s) == SQL_SUCCESS:
        n += 1
    assert n == 5
    free(s)


# ---- parameters ------------------------------------------------------------------------


def test_output_parameter(conn):
    s = conn.stmt()
    inp = ctypes.c_int32(7)
    out = ctypes.c_double(0)
    out_ind = SQLLEN(0)
    check(odbc.SQLBindParameter(s, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, ctypes.byref(inp), 0, None), s)
    check(odbc.SQLBindParameter(s, 2, SQL_PARAM_OUTPUT, SQL_C_DOUBLE, SQL_DECIMAL, 11, 2, ctypes.byref(out), 8,
                                ctypes.byref(out_ind)), s)
    exec_direct(s, "CALL ODBCTEST.GET_TOTAL(?, ?)")
    assert out.value == 1234.5 and out_ind.value == 8
    free(s)


def test_data_at_execution(conn, unique):
    s = conn.stmt()
    exec_direct(s, f"CREATE SCHEMA D{unique}")
    exec_direct(s, f"CREATE TABLE D{unique}.T (ID INTEGER, TXT CLOB)")
    ident = ctypes.c_int32(5)
    token = ctypes.c_void_p(0xBEEF)
    ind = SQLLEN(-100)  # SQL_LEN_DATA_AT_EXEC(0)
    check(odbc.SQLBindParameter(s, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, ctypes.byref(ident), 0, None), s)
    check(odbc.SQLBindParameter(s, 2, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_LONGVARCHAR, 100000, 0, token, 0,
                                ctypes.byref(ind)), s)
    rc = odbc.SQLExecDirectW(s, wstr(f"INSERT INTO D{unique}.T VALUES (?, ?)"), SQL_NTS)
    assert rc == SQL_NEED_DATA
    which = ctypes.c_void_p()
    assert odbc.SQLParamData(s, ctypes.byref(which)) == SQL_NEED_DATA
    assert which.value == 0xBEEF
    for piece in ["first part, ", "second é€ part, ", "end"]:
        data = piece.encode("utf-16-le")
        check(odbc.SQLPutData(s, ctypes.create_string_buffer(data, len(data)), SQLLEN(len(data))), s)
    check(odbc.SQLParamData(s, ctypes.byref(which)), s)
    rowcount = SQLLEN()
    check(odbc.SQLRowCount(s, ctypes.byref(rowcount)), s)
    assert rowcount.value == 1
    odbc.SQLFreeStmt(s, 3)  # SQL_RESET_PARAMS
    exec_direct(s, f"SELECT TXT FROM D{unique}.T WHERE ID = 5")
    check(odbc.SQLFetch(s), s)
    rc, buf, ind2 = get_data(s, 1, SQL_C_WCHAR, 400)
    assert buf.raw[:ind2].decode("utf-16-le") == "first part, second é€ part, end"
    free(s)


def test_parameter_arrays_with_status(conn, unique):
    s = conn.stmt()
    exec_direct(s, f"CREATE SCHEMA P{unique}")
    exec_direct(s, f"CREATE TABLE P{unique}.T (ID INTEGER, NAME VARCHAR(10))")
    n = 4
    ids = (ctypes.c_int32 * n)(1, 2, 3, 4)
    names = ctypes.create_string_buffer(b"one\0\0\0two\0\0\0three\0four\0\0", 6 * n)
    name_ind = (SQLLEN * n)(SQL_NTS, SQL_NTS, SQL_NULL_DATA, SQL_NTS)
    status = (ctypes.c_uint16 * n)()
    ops = (ctypes.c_uint16 * n)(0, SQL_PARAM_IGNORE, 0, 0)
    processed = SQLULEN()
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_PARAMSET_SIZE, H(n), 0), s)
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_PARAM_STATUS_PTR, status, 0), s)
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_PARAM_OPERATION_PTR, ops, 0), s)
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_PARAMS_PROCESSED_PTR, ctypes.byref(processed), 0), s)
    check(odbc.SQLBindParameter(s, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, ids, 0, None), s)
    check(odbc.SQLBindParameter(s, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 10, 0, names, SQLLEN(6), name_ind), s)
    exec_direct(s, f"INSERT INTO P{unique}.T VALUES (?, ?)")
    assert processed.value == 3
    assert list(status) == [SQL_PARAM_SUCCESS, SQL_PARAM_UNUSED, SQL_PARAM_SUCCESS, SQL_PARAM_SUCCESS]
    rowcount = SQLLEN()
    check(odbc.SQLRowCount(s, ctypes.byref(rowcount)), s)
    assert rowcount.value == 3
    check(odbc.SQLSetStmtAttrW(s, SQL_ATTR_PARAMSET_SIZE, H(1), 0), s)
    odbc.SQLFreeStmt(s, 3)
    exec_direct(s, f"SELECT ID, NAME FROM P{unique}.T ORDER BY ID")
    got = []
    while odbc.SQLFetch(s) == SQL_SUCCESS:
        rc, b1, _ = get_data(s, 1, SQL_C_SLONG, 4)
        rc, b2, i2 = get_data(s, 2, SQL_C_CHAR, 16)
        got.append((struct.unpack("i", b1.raw)[0], None if i2 == SQL_NULL_DATA else b2.value.decode()))
    assert got == [(1, "one"), (3, None), (4, "four")]
    free(s)


# ---- diagnostics and state -------------------------------------------------------------


def test_diagnostics(conn):
    s = conn.stmt()
    rc = odbc.SQLExecDirectW(s, wstr("SELECT * FROM NOPE.NOPE"), SQL_NTS)
    assert rc == SQL_ERROR
    recs = diag(SQL_HANDLE_STMT, s)
    assert len(recs) == 1
    state, native, msg = recs[0]
    assert state == "42704" and native == -204
    assert msg.startswith("[Mapepire][ODBC Driver][Db2 for i]")
    count = ctypes.c_int32()
    check(odbc.SQLGetDiagFieldW(SQL_HANDLE_STMT, s, 0, SQL_DIAG_NUMBER, ctypes.byref(count), 0, None), s)
    assert count.value == 1
    origin = ctypes.create_string_buffer(64)
    ln = ctypes.c_int16()
    check(odbc.SQLGetDiagFieldW(SQL_HANDLE_STMT, s, 1, SQL_DIAG_CLASS_ORIGIN, origin, 64, ctypes.byref(ln)), s)
    assert origin.raw[: ln.value].decode("utf-16-le") == "ISO 9075"
    free(s)


def test_cursor_state_errors(conn):
    s = conn.stmt()
    assert odbc.SQLCloseCursor(s) == SQL_ERROR and states(s) == ["24000"]
    exec_direct(s, "SELECT * FROM SAMPLE.DEPARTMENT")
    assert odbc.SQLExecDirectW(s, wstr("SELECT * FROM SAMPLE.DEPARTMENT"), SQL_NTS) == SQL_ERROR
    assert states(s) == ["24000"]
    assert odbc.SQLMoreResults(s) == SQL_NO_DATA
    # the cursor was closed by SQLMoreResults; the Driver Manager rejects the fetch (HY010)
    assert odbc.SQLFetch(s) == SQL_ERROR and states(s)[0] in ("HY010", "24000")
    assert odbc.SQLSetStmtAttrW(s, SQL_ATTR_CURSOR_TYPE, H(SQL_CURSOR_STATIC), 0) == SQL_SUCCESS_WITH_INFO
    assert states(s) == ["01S02"]
    free(s)


def test_get_functions(conn):
    bitmap = (ctypes.c_uint16 * 250)()
    check(odbc.SQLGetFunctions(conn.dbc, SQL_API_ODBC3_ALL_FUNCTIONS, bitmap), conn.dbc, SQL_HANDLE_DBC)

    def has(fid):
        return bool(bitmap[fid >> 4] & (1 << (fid & 0xF)))

    assert has(SQL_API_SQLDESCRIBEPARAM)
    assert not has(SQL_API_SQLBROWSECONNECT)


def test_native_sql(conn):
    out = ctypes.create_string_buffer(200)
    ln = ctypes.c_int32()
    sql = "SELECT {fn UCASE(DEPTNAME)} FROM SAMPLE.DEPARTMENT"
    check(odbc.SQLNativeSqlW(conn.dbc, wstr(sql), SQL_NTS, out, 100, ctypes.byref(ln)), conn.dbc, SQL_HANDLE_DBC)
    assert out.raw[: ln.value * 2].decode("utf-16-le") == sql


@pytest.mark.skipif(sys.platform != "win32", reason="ANSI code page conversion is Windows-only")
def test_ansi_application_gets_code_page_text(connstr):
    """An ANSI application (SQLDriverConnectA) receives SQL_C_CHAR data in its code page."""
    import locale
    acp = "cp" + str(ctypes.windll.kernel32.GetACP())
    expected = "héllo wörld ☃"
    for ansi, encoding in ((True, acp), (False, "utf-8")):
        c = Conn(connstr(), ansi=ansi)
        s = c.stmt()
        exec_direct(s, "SELECT * FROM ODBCTEST.TYPED")
        check(odbc.SQLFetch(s), s)
        rc, buf, ind = get_data(s, 6, SQL_C_CHAR, 100)
        assert buf.raw[:ind] == expected.encode(encoding, errors="replace"), (ansi, buf.raw[:ind])
        free(s)
        c.close()
    del locale

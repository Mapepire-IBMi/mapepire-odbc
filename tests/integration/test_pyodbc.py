"""
End-to-end tests through the ODBC Driver Manager using pyodbc, against the
Mapepire mock server.  Statements that no mock rule answers run in the mock's
in-memory SQLite database, which is shared by every connection.
"""
import datetime
import decimal
import threading

import pyodbc
import pytest


@pytest.fixture()
def cn(connstr):
    c = pyodbc.connect(connstr(), autocommit=True)
    yield c
    c.close()


def sqlstate(exc):
    return exc.args[0] if exc.args else None


# ---- connecting -------------------------------------------------------------------


def test_connect_and_getinfo(cn):
    assert cn.getinfo(pyodbc.SQL_DBMS_NAME) == "DB2/400 SQL"
    assert cn.getinfo(pyodbc.SQL_DBMS_VER) == "00.02.0000"  # mock server 0.2.0
    assert cn.getinfo(pyodbc.SQL_DRIVER_ODBC_VER) == "03.80"
    assert cn.getinfo(pyodbc.SQL_USER_NAME) == "MOCK"
    assert cn.getinfo(pyodbc.SQL_DATABASE_NAME) == "MOCKDB"
    assert cn.getinfo(pyodbc.SQL_IDENTIFIER_QUOTE_CHAR) == '"'
    assert cn.getinfo(pyodbc.SQL_TXN_CAPABLE) == 2  # SQL_TC_ALL
    assert cn.getinfo(pyodbc.SQL_MAX_SCHEMA_NAME_LEN) == 128


def test_user_name_is_case_insensitive(connstr):
    pyodbc.connect(connstr(uid="mock"), autocommit=True).close()


def test_bad_password_is_28000(connstr):
    with pytest.raises(pyodbc.Error) as ei:
        pyodbc.connect(connstr(pwd="Mockingbird"))
    assert sqlstate(ei.value) == "28000"


def test_unpinned_self_signed_certificate_is_rejected(connstr):
    with pytest.raises(pyodbc.Error) as ei:
        pyodbc.connect(connstr(certsha256=None))
    assert sqlstate(ei.value) == "08001"
    assert "TrustServerCertificate" in str(ei.value) or "CertSHA256" in str(ei.value)


def test_wrong_pin_is_rejected(connstr):
    with pytest.raises(pyodbc.Error) as ei:
        pyodbc.connect(connstr(certsha256="00" * 32))
    assert sqlstate(ei.value) == "08001"
    assert "does not match" in str(ei.value)


def test_trust_server_certificate(connstr):
    c = pyodbc.connect(connstr(certsha256=None, trustservercertificate="yes"), autocommit=True)
    assert c.cursor().execute("SELECT * FROM SYSIBM.SYSDUMMY1").fetchone()[0] == "Y"
    c.close()


def test_missing_server(driver_conn_part):
    with pytest.raises(pyodbc.Error) as ei:
        pyodbc.connect(driver_conn_part + ";UID=MOCK;PWD=mockingbird")
    assert sqlstate(ei.value) == "08001"


def test_connection_refused(driver_conn_part):
    with pytest.raises(pyodbc.Error) as ei:
        pyodbc.connect(driver_conn_part + ";SERVER=127.0.0.1;PORT=1;UID=MOCK;PWD=x;CONNECTTIMEOUT=5")
    assert sqlstate(ei.value) == "08001"


@pytest.mark.parametrize("transport", ["default", "socket"])
def test_plain_websocket_transports(ws_connstr, transport):
    c = pyodbc.connect(ws_connstr(transport=None if transport == "default" else transport), autocommit=True)
    rows = c.cursor().execute("SELECT * FROM SAMPLE.DEPARTMENT").fetchall()
    assert len(rows) == 14
    c.close()


# ---- queries ------------------------------------------------------------------------


def test_sample_department(cn):
    cur = cn.cursor()
    cur.execute("SELECT * FROM SAMPLE.DEPARTMENT")
    assert [d[0] for d in cur.description] == ["DEPTNO", "DEPTNAME", "MGRNO", "ADMRDEPT", "LOCATION"]
    # (name, type_code, display_size, internal_size, precision, scale, null_ok)
    assert cur.description[0][3] == 3 and cur.description[0][6] is False
    assert cur.description[4][6] is True
    rows = cur.fetchall()
    assert len(rows) == 14
    assert tuple(rows[0]) == ("A00", "SPIFFY COMPUTER SERVICE DIV.", "000010", "A00", None)
    assert cur.rowcount == -1


@pytest.mark.parametrize("block", [1, 3, 5, 1000])
def test_paging_with_sqlmore(connstr, block):
    c = pyodbc.connect(connstr(blocksize=block), autocommit=True)
    rows = c.cursor().execute("SELECT * FROM SAMPLE.DEPARTMENT").fetchall()
    assert [r[0] for r in rows][:3] == ["A00", "B01", "C01"]
    assert len(rows) == 14
    c.close()


def test_parameterized_rule(cn):
    cur = cn.cursor()
    assert len(cur.execute("SELECT * FROM SAMPLE.DEPARTMENT WHERE ADMRDEPT = ?", "A00").fetchall()) == 5
    assert len(cur.execute("SELECT * FROM SAMPLE.DEPARTMENT WHERE ADMRDEPT = ?", "E01").fetchall()) == 7
    assert cur.execute("SELECT * FROM SAMPLE.DEPARTMENT WHERE ADMRDEPT = ?", "ZZZ").fetchall() == []


def test_interleaved_cursors_on_one_connection(connstr):
    c = pyodbc.connect(connstr(blocksize=2), autocommit=True)
    a = c.cursor().execute("SELECT * FROM SAMPLE.DEPARTMENT")
    b = c.cursor().execute("SELECT * FROM SAMPLE.DEPARTMENT WHERE ADMRDEPT = ?", "E01")
    got_a, got_b = [], []
    while True:
        ra, rb = a.fetchone(), b.fetchone()
        if ra:
            got_a.append(ra[0])
        if rb:
            got_b.append(rb[0])
        if not ra and not rb:
            break
    assert len(got_a) == 14 and len(got_b) == 7
    c.close()


def test_threads_with_separate_connections(connstr):
    errors = []

    def work():
        try:
            c = pyodbc.connect(connstr(blocksize=4), autocommit=True)
            for _ in range(5):
                assert len(c.cursor().execute("SELECT * FROM SAMPLE.DEPARTMENT").fetchall()) == 14
            c.close()
        except Exception as e:  # pragma: no cover - reported below
            errors.append(e)

    threads = [threading.Thread(target=work) for _ in range(6)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors


def test_typed_values(cn):
    cur = cn.cursor()
    cur.execute("SELECT * FROM ODBCTEST.TYPED")
    types = {d[0]: d[1] for d in cur.description}
    assert types["I"] is int and types["DBL"] is float and types["D"] is datetime.date
    assert types["TS"] is datetime.datetime and types["BO"] is bool and types["BIN"] is bytearray
    r = cur.fetchone()
    assert r.I == 42 and r.SI == -7
    assert r.BI == 9007199254740993  # beyond 2**53: must not pass through a double
    assert r.DBL == 2.5
    assert r.C == "ABC"
    assert r.VC == "héllo wörld ☃"
    assert r.D == datetime.date(2024, 2, 29)
    assert r.T == datetime.time(13, 45, 30)
    assert r.TS == datetime.datetime(2024, 2, 29, 13, 45, 30, 123456)
    assert r.BO is True
    assert bytes(r.BIN) == bytes.fromhex("DEADBEEF")
    r = cur.fetchone()
    assert r.I == -1 and r.SI is None and r.TS is None and r.BO is False and r.BIN is None
    assert cur.fetchone() is None


def test_decimal_precision_is_preserved(cn):
    rows = cn.cursor().execute("SELECT * FROM ODBCTEST.PRECISE").fetchall()
    assert rows[0].BIG == decimal.Decimal("12345678901234567890.123456789")
    assert rows[0].TINY == decimal.Decimal("0.0000001")
    assert rows[0].NUM == decimal.Decimal("123.45")
    assert rows[1].BIG == decimal.Decimal("-0.5") and rows[1].TINY is None


def test_lob_url_failure_is_reported(cn):
    # The mock has no /blob endpoint, so the download must fail cleanly (not crash or return junk).
    cur = cn.cursor().execute("SELECT * FROM ODBCTEST.LOB")
    with pytest.raises(pyodbc.Error) as ei:
        cur.fetchone()
    assert "LOB" in str(ei.value) and "404" in str(ei.value)


# ---- DML / DDL in the SQLite fallback ------------------------------------------------


@pytest.fixture()
def schema(cn, unique):
    name = "S" + unique
    cn.cursor().execute(f"CREATE SCHEMA {name}")
    return name


def test_ddl_dml_roundtrip(cn, schema):
    cur = cn.cursor()
    cur.execute(f"""CREATE TABLE {schema}.T (ID INTEGER NOT NULL, NAME VARCHAR(100), AMOUNT DECIMAL(11,2),
                    RATE DOUBLE, BORN DATE, SEEN TIMESTAMP, ACTIVE BOOLEAN, BIG BIGINT, PIC BLOB)""")
    rows = [
        (1, "Ann", decimal.Decimal("10.50"), 0.25, datetime.date(1990, 1, 2),
         datetime.datetime(2024, 5, 6, 7, 8, 9, 123000), True, 2**62, b"\x00\x01\x02\xff"),
        (2, "Émilie 日本 \U0001f600", decimal.Decimal("-3.00"), -1e10, None, None, False, -5, None),
        (3, None, None, None, None, None, None, None, None),
    ]
    for r in rows:
        cur.execute(f"INSERT INTO {schema}.T VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", *r)
        assert cur.rowcount == 1
    got = cur.execute(f"SELECT ID, NAME, AMOUNT, RATE, BORN, SEEN, ACTIVE, BIG, PIC FROM {schema}.T ORDER BY ID").fetchall()
    assert len(got) == 3
    g = got[0]
    assert g.ID == 1 and g.NAME == "Ann" and g.AMOUNT == decimal.Decimal("10.50") and g.RATE == 0.25
    assert g.BORN == datetime.date(1990, 1, 2)
    assert g.SEEN == datetime.datetime(2024, 5, 6, 7, 8, 9, 123000)
    assert g.ACTIVE is True and g.BIG == 2**62 and bytes(g.PIC) == b"\x00\x01\x02\xff"
    assert got[1].NAME == "Émilie 日本 \U0001f600"
    assert got[1].AMOUNT == decimal.Decimal("-3") and got[1].RATE == -1e10 and got[1].ACTIVE is False
    assert all(v is None for v in tuple(got[2])[1:])

    assert cur.execute(f"UPDATE {schema}.T SET NAME = ? WHERE ID > ?", "x", 1).rowcount == 2
    assert cur.execute(f"DELETE FROM {schema}.T WHERE ID = 3").rowcount == 1
    assert cur.execute(f"SELECT COUNT(*) FROM {schema}.T").fetchone()[0] == 2


def test_executemany_uses_batches(cn, schema):
    cur = cn.cursor()
    cur.execute(f"CREATE TABLE {schema}.M (ID INTEGER, LABEL VARCHAR(20))")
    cur.fast_executemany = True
    data = [(i, f"row {i}") for i in range(250)]
    cur.executemany(f"INSERT INTO {schema}.M VALUES (?, ?)", data)
    assert tuple(cur.execute(f"SELECT COUNT(*), SUM(ID) FROM {schema}.M").fetchone()) == (250, sum(range(250)))
    cur.fast_executemany = False
    cur.executemany(f"INSERT INTO {schema}.M VALUES (?, ?)", [(1000, "a"), (1001, "b")])
    assert cur.execute(f"SELECT COUNT(*) FROM {schema}.M").fetchone()[0] == 252


def test_long_values_use_chunked_getdata(cn, schema):
    cur = cn.cursor()
    cur.execute(f"CREATE TABLE {schema}.L (TXT CLOB, BIN BLOB)")
    text = ("0123456789é€\U0001f600" * 20000)[:200000]
    blob = bytes(range(256)) * 400
    cur.execute(f"INSERT INTO {schema}.L VALUES (?, ?)", text, blob)
    row = cur.execute(f"SELECT TXT, BIN FROM {schema}.L").fetchone()
    assert row.TXT == text
    assert bytes(row.BIN) == blob


def test_empty_string_vs_null(cn, schema):
    cur = cn.cursor()
    cur.execute(f"CREATE TABLE {schema}.E (V VARCHAR(10))")
    cur.execute(f"INSERT INTO {schema}.E VALUES (?)", "")
    cur.execute(f"INSERT INTO {schema}.E VALUES (?)", None)
    vals = [r[0] for r in cur.execute(f"SELECT V FROM {schema}.E").fetchall()]
    assert vals == ["", None]


def test_sql_error_mapping(cn):
    cur = cn.cursor()
    with pytest.raises(pyodbc.ProgrammingError) as ei:
        cur.execute("SELECT * FROM NOSUCH.TABLE1")
    assert sqlstate(ei.value) == "42704"
    assert "[Db2 for i]" in str(ei.value) and "(-204)" in str(ei.value)
    with pytest.raises(pyodbc.Error) as ei:
        cur.execute("DROP TABLE ODBCTEST.PROTECTED")
    assert sqlstate(ei.value) == "42501" and "(-551)" in str(ei.value)
    # the connection is still usable afterwards
    assert cur.execute("SELECT * FROM SYSIBM.SYSDUMMY1").fetchval() == "Y"


def test_parameter_count_mismatch(cn):
    with pytest.raises(pyodbc.Error):
        cn.cursor().execute("SELECT * FROM SAMPLE.DEPARTMENT WHERE ADMRDEPT = ?")


def test_transactions_and_autocommit_toggle(connstr, unique):
    c = pyodbc.connect(connstr(), autocommit=True)
    cur = c.cursor()
    cur.execute(f"CREATE SCHEMA X{unique}")
    cur.execute(f"CREATE TABLE X{unique}.T (A INTEGER)")
    c.autocommit = False  # reconnects with "auto commit=false"
    assert c.autocommit is False
    cur = c.cursor()
    cur.execute(f"INSERT INTO X{unique}.T VALUES (1)")
    c.commit()
    c.rollback()
    c.autocommit = True
    assert cur.execute(f"SELECT COUNT(*) FROM X{unique}.T").fetchval() == 1
    c.close()


def test_autocommit_off_from_the_start(connstr):
    c = pyodbc.connect(connstr(), autocommit=False)
    assert c.cursor().execute("SELECT * FROM SYSIBM.SYSDUMMY1").fetchval() == "Y"
    c.commit()
    c.close()


# ---- catalog functions --------------------------------------------------------------


@pytest.fixture(scope="module")
def sysibm_views(connstr):
    """Emulate the SYSIBM catalog views in the mock's SQLite store."""
    c = pyodbc.connect(connstr(), autocommit=True)
    cur = c.cursor()
    try:
        cur.execute("CREATE SCHEMA SYSIBM")
    except pyodbc.Error:
        pass
    cur.execute("""CREATE TABLE IF NOT EXISTS SYSIBM.SQLTABLES (TABLE_CAT VARCHAR(18), TABLE_SCHEM VARCHAR(128),
                   TABLE_NAME VARCHAR(128), TABLE_TYPE VARCHAR(30), REMARKS VARCHAR(2000), TABLE_TEXT VARCHAR(50))""")
    cur.execute("""CREATE TABLE IF NOT EXISTS SYSIBM.SQLCOLUMNS (TABLE_CAT VARCHAR(18), TABLE_SCHEM VARCHAR(128),
                   TABLE_NAME VARCHAR(128), COLUMN_NAME VARCHAR(128), DATA_TYPE SMALLINT, TYPE_NAME VARCHAR(261),
                   COLUMN_SIZE INTEGER, BUFFER_LENGTH INTEGER, DECIMAL_DIGITS SMALLINT, NUM_PREC_RADIX SMALLINT,
                   NULLABLE SMALLINT, REMARKS VARCHAR(2000), COLUMN_TEXT VARCHAR(50), COLUMN_DEF VARCHAR(2000),
                   SQL_DATA_TYPE SMALLINT, SQL_DATETIME_SUB SMALLINT, CHAR_OCTET_LENGTH INTEGER,
                   ORDINAL_POSITION INTEGER, IS_NULLABLE VARCHAR(3))""")
    cur.execute("""CREATE TABLE IF NOT EXISTS SYSIBM.SQLPRIMARYKEYS (TABLE_CAT VARCHAR(18), TABLE_SCHEM VARCHAR(128),
                   TABLE_NAME VARCHAR(128), COLUMN_NAME VARCHAR(128), KEY_SEQ SMALLINT, PK_NAME VARCHAR(128))""")
    cur.execute("""CREATE TABLE IF NOT EXISTS SYSIBM.SQLSCHEMAS (TABLE_CAT VARCHAR(18), TABLE_SCHEM VARCHAR(128))""")
    cur.execute("DELETE FROM SYSIBM.SQLTABLES")
    cur.execute("DELETE FROM SYSIBM.SQLCOLUMNS")
    cur.execute("DELETE FROM SYSIBM.SQLPRIMARYKEYS")
    cur.execute("DELETE FROM SYSIBM.SQLSCHEMAS")
    tables = [("MOCKDB", "SALES", "ORDERS", "TABLE", None, "Customer orders"),
              ("MOCKDB", "SALES", "ORDER_VIEW", "VIEW", "Orders view", ""),
              ("MOCKDB", "HR", "EMP", "TABLE", None, None),
              ("MOCKDB", "QSYS2", "SYSTABLES", "SYSTEM TABLE", None, None)]
    cur.executemany("INSERT INTO SYSIBM.SQLTABLES VALUES (?, ?, ?, ?, ?, ?)", tables)
    # DATA_TYPE values are deliberately the CLI codes IBM i reports (e.g. -98 for BLOB, -2 for FOR BIT DATA)
    cols = [("MOCKDB", "SALES", "ORDERS", "ID", 4, "INTEGER", 10, 4, 0, 10, 0, None, "Order id", None, 4, None, None, 1, "NO"),
            ("MOCKDB", "SALES", "ORDERS", "TOTAL", 3, "DECIMAL", 11, 13, 2, 10, 1, None, None, "0", 3, None, None, 2, "YES"),
            ("MOCKDB", "SALES", "ORDERS", "PLACED", 93, "TIMESTAMP", 26, 16, 6, None, 1, None, None, None, 9, 3, None, 3, "YES"),
            ("MOCKDB", "SALES", "ORDERS", "TOKEN", 1, "CHAR () FOR BIT DATA", 16, 16, None, None, 1, None, None, None, 1, None, 16, 4, "YES"),
            ("MOCKDB", "SALES", "ORDERS", "PHOTO", -98, "BLOB", 1048576, 1048576, None, None, 1, None, None, None, -98, None, 1048576, 5, "YES")]
    cur.executemany("INSERT INTO SYSIBM.SQLCOLUMNS VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", cols)
    cur.execute("INSERT INTO SYSIBM.SQLPRIMARYKEYS VALUES ('MOCKDB', 'SALES', 'ORDERS', 'ID', 1, 'ORDERS_PK')")
    cur.executemany("INSERT INTO SYSIBM.SQLSCHEMAS VALUES (?, ?)", [("MOCKDB", "SALES"), ("MOCKDB", "HR")])
    yield
    c.close()


def test_sqltables(cn, sysibm_views):
    cur = cn.cursor()
    rows = cur.tables(schema="SALES").fetchall()
    assert [(r.table_name, r.table_type) for r in rows] == [("ORDERS", "TABLE"), ("ORDER_VIEW", "VIEW")]
    assert rows[0].remarks == "Customer orders" and rows[1].remarks == "Orders view"
    assert [r.table_name for r in cur.tables(tableType="VIEW").fetchall()] == ["ORDER_VIEW"]
    assert sorted(r.table_name for r in cur.tables(tableType="'TABLE','VIEW'").fetchall()) == \
        ["EMP", "ORDERS", "ORDER_VIEW"]
    assert [r.table_name for r in cur.tables(table="ORDER%", schema="S%").fetchall()] == ["ORDERS", "ORDER_VIEW"]
    # an escaped underscore matches only a literal underscore
    assert [r.table_name for r in cur.tables(table="ORDER\\_%").fetchall()] == ["ORDER_VIEW"]


def test_sqltables_enumerations(cn, sysibm_views):
    cur = cn.cursor()
    assert [r.table_cat for r in cur.tables(catalog="%", schema="", table="").fetchall()] == ["MOCKDB"]
    assert [r.table_schem for r in cur.tables(catalog="", schema="%", table="").fetchall()] == ["HR", "SALES"]
    types = [r.table_type for r in cur.tables(catalog="", schema="", table="", tableType="%").fetchall()]
    assert "TABLE" in types and "VIEW" in types


def test_sqlcolumns_type_codes_are_odbc(cn, sysibm_views):
    rows = cn.cursor().columns(table="ORDERS", schema="SALES").fetchall()
    assert [r.column_name for r in rows] == ["ID", "TOTAL", "PLACED", "TOKEN", "PHOTO"]
    by = {r.column_name: r for r in rows}
    assert by["ID"].data_type == pyodbc.SQL_INTEGER and by["ID"].nullable == 0 and by["ID"].remarks == "Order id"
    assert by["TOTAL"].data_type == pyodbc.SQL_DECIMAL and by["TOTAL"].decimal_digits == 2
    assert by["PLACED"].data_type == pyodbc.SQL_TYPE_TIMESTAMP
    assert by["PLACED"].sql_data_type == 9 and by["PLACED"].sql_datetime_sub == 3
    assert by["TOKEN"].data_type == pyodbc.SQL_BINARY  # remapped from the CLI code
    assert by["PHOTO"].data_type == pyodbc.SQL_LONGVARBINARY  # remapped from -98


def test_sqlprimarykeys(cn, sysibm_views):
    rows = cn.cursor().primaryKeys("ORDERS", schema="SALES").fetchall()
    assert [(r.column_name, r.key_seq, r.pk_name) for r in rows] == [("ID", 1, "ORDERS_PK")]


def test_sqlgettypeinfo(cn):
    rows = cn.cursor().getTypeInfo().fetchall()
    names = [r.type_name for r in rows]
    assert "VARCHAR" in names and "DECIMAL" in names and "TIMESTAMP" in names
    assert [r.data_type for r in rows] == sorted(r.data_type for r in rows)
    ts = cn.cursor().getTypeInfo(pyodbc.SQL_TYPE_TIMESTAMP).fetchall()
    assert [r.type_name for r in ts] == ["TIMESTAMP"]

# Mapepire ODBC Driver

> [!CAUTION]
> ## 🚨 Vibe-code Alert!!! 🚨
> This driver was conjured up in a single sitting by an AI, mostly in C, with nobody holding its beer.
> It has tests (lots of them!) and it was reviewed (by another AI!), but treat it with the healthy suspicion
> you'd give a parachute packed by a very enthusiastic stranger: lovely to look at, but check it before you jump.

> [!WARNING]
> **This project is experimental.** It has been tested against the
> [Mapepire mock server](https://github.com/Mapepire-IBMi/mapepire-mockserver) v0.2.0, not yet against a real
> Mapepire server on IBM i. Expect rough edges and report what you find.

An ODBC 3.8 driver for Db2 for IBM i that talks the
[Mapepire WebSocket protocol](https://github.com/Mapepire-IBMi/mapepire-protocol) to a
[Mapepire server](https://github.com/Mapepire-IBMi/mapepire-server). Any ODBC application can use it, including
Excel, Power BI, pyodbc, R `odbc` and unixODBC `isql`. No IBM i Access Client Solutions install is needed: the only
network requirement is a WebSocket connection to the Mapepire server (port 8076 by default).

- Unicode driver written in portable C11, with no third-party dependencies on Windows.
- TLS by default. Supports certificate pinning (`CertSHA256`) for the self-signed certificates Mapepire servers
  usually have.
- Parameters, parameter arrays (batches), data-at-execution, output parameters, block (array) fetches and
  chunked `SQLGetData`.
- Catalog functions (`SQLTables`, `SQLColumns`, `SQLPrimaryKeys`, ...) backed by the Db2 for i `SYSIBM` catalog
  views.
- Exact `DECIMAL`/`NUMERIC` values: numbers are never rounded through a `double` on their way to the application.

## Building

You need CMake 3.16 or later and a C11 compiler.

**Windows** (Visual Studio 2019 or later, or MinGW-w64 / clang):

```sh
cmake -S . -B build -A x64
cmake --build build --config Release
```

This produces `build\Release\mapepireodbc.dll`. It links only system libraries: WinHTTP, Winsock, CryptoAPI and
`odbccp32`. Build `-A Win32` for 32-bit applications. The DLL's bitness must match the application that loads it.

**Linux / macOS** (unixODBC and OpenSSL development packages, e.g. `apt install unixodbc-dev libssl-dev`):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build      # installs lib/libmapepireodbc.so
```

| CMake option | Default | Meaning |
|---|---|---|
| `MAPEPIRE_WITH_OPENSSL` | `ON` except on Windows | TLS for the portable socket transport. On Windows, TLS is provided by WinHTTP. |
| `MAPEPIRE_BUILD_TESTS` | `ON` | Build the `unit_tests` executable. |

## Installing

### Windows

From an elevated PowerShell, register the driver for the machine:

```powershell
.\scripts\register-windows.ps1 -InstallDriver -DllPath C:\path\to\mapepireodbc.dll
```

You can then connect with `DRIVER={Mapepire ODBC Driver};SERVER=...`.

Without administrator rights, create a user DSN that points directly at the DLL:

```powershell
.\scripts\register-windows.ps1 -AddUserDsn -Name MyIBMi -DllPath .\build\Release\mapepireodbc.dll `
    -Server myibmi.example.com -CertSHA256 '21:37:DA:8C:...'
```

Then connect with `DSN=MyIBMi;UID=...;PWD=...`. For Excel, see [docs/EXCEL.md](docs/EXCEL.md). The driver has no configuration dialog: DSN attributes are plain
registry values named after the keywords below.

### Linux / macOS (unixODBC)

Edit the path in [scripts/odbcinst.ini.template](scripts/odbcinst.ini.template), then run
`sudo odbcinst -i -d -f scripts/odbcinst.ini.template`. Alternatively, skip registration and use
`DRIVER=/path/to/libmapepireodbc.so` in the connection string.

## Connecting

```python
import pyodbc
cn = pyodbc.connect("DRIVER={Mapepire ODBC Driver};SERVER=myibmi;UID=me;PWD=secret;"
                    "CertSHA256=21:37:DA:8C:...;Props={naming=system;libraries=MYLIB,QGPL}")
for row in cn.cursor().execute("SELECT * FROM QSYS2.SYSTABLES FETCH FIRST 5 ROWS ONLY"):
    print(row)
```

### Connection string / DSN keywords

Keywords are case-insensitive. Aliases are listed in parentheses. Values may be wrapped in `{...}` to include `;`.

| Keyword | Default | Description |
|---|---|---|
| `SERVER` (`HOST`, `SYSTEM`) | required | Host name or address of the Mapepire server. |
| `PORT` | `8076` | Mapepire server port. |
| `UID` (`USER`) / `PWD` (`PASSWORD`) | required | IBM i user profile and password, sent with HTTP Basic authentication over TLS. |
| `SECURE` (`SSL`) | `yes` | `no` uses plain `ws://`, which only works if the server runs with `MP_UNSECURE=true`. This sends the password in clear text. |
| `CERTSHA256` | | SHA-256 fingerprint of the server certificate (hex digits; `:` separators allowed). This replaces CA validation, and the connection is refused before credentials are sent if the fingerprint does not match. This is the recommended setting for self-signed certificates. |
| `TRUSTSERVERCERTIFICATE` | `no` | `yes` skips certificate validation entirely. This is insecure; prefer `CERTSHA256`. |
| `CAFILE` | | PEM CA bundle (socket/OpenSSL transport only). On Windows, install the CA in the certificate store instead. |
| `PROPS` (`JDBCOPTIONS`) | | JTOpen JDBC properties for the server-side connection, e.g. `naming=system;libraries=MYLIB;date format=iso`. |
| `NAMING` | | Shortcut for the `naming` property (`sql` or `system`). |
| `LIBRARIES` (`DBQ`) | | Shortcut for the `libraries` property (library list / default schema). |
| `TECHNIQUE` | server default (`cli`) | JDBC connection technique: `cli` or `tcp`. |
| `APPLICATION` | `mapepire-odbc` | Application name reported to the server. |
| `AUTOCOMMIT` | `yes` | Initial autocommit mode. It can also be changed later with `SQL_ATTR_AUTOCOMMIT`. |
| `READONLY` | `no` | `yes` requests a read-only connection (`access=read only`). |
| `BLOCKSIZE` (`FETCHSIZE`) | `1000` | Rows fetched per round trip. |
| `CONNECTTIMEOUT` | `30` | Seconds to wait when connecting. `SQL_ATTR_LOGIN_TIMEOUT` overrides it. |
| `SOCKETTIMEOUT` | `0` (none) | Seconds to wait for each server reply (and for sends). A request that times out fails with `HYT00` and leaves the connection usable. `SQL_ATTR_QUERY_TIMEOUT` overrides it per statement. |
| `TRANSPORT` | `winhttp` on Windows, `socket` elsewhere | WebSocket implementation. `socket` on Windows supports plain `ws://` only. |
| `CHARENCODING` | automatic | Encoding of `SQL_C_CHAR` data: `utf8` or `ansi` (the Windows ANSI code page). By default, ANSI applications on Windows get their code page and every other application gets UTF-8. |
| `PATH` | `/db/` | WebSocket path on the server. |

By default the driver adds `date format=iso;time format=iso` to `PROPS`, so date and time values are unambiguous.
Values you set in `PROPS` take precedence.

To find a server's fingerprint:
`openssl s_client -connect myibmi:8076 </dev/null | openssl x509 -noout -fingerprint -sha256`.

## How it maps onto Mapepire

- **Concurrency:** a connection can be used by many threads at once. Each connection runs a reader thread that
  routes every reply to the request with the matching `id`, much like the EventEmitter in the JavaScript client.
  Statements on different threads therefore have requests in flight at the same time, and a slow query on one
  statement does not hold up the others. A single statement handle should still be used by one thread at a time.
- **Statements without parameters** run with a `sql` request. **Statements with parameters** run with a fresh
  `prepare_sql_execute` on every execution. The real server cannot page (`sqlmore`) the results of the separate
  `execute` message, so it is never used.
- **`SQLPrepare` is deferred**: no round trip happens until the statement runs. If the application asks for metadata
  first (`SQLNumResultCols`, `SQLDescribeCol`, `SQLDescribeParam`), the driver sends `prepare_sql` and closes that
  server handle straight away. `SQLNumParams` counts parameter markers locally.
- **Rows** are requested in terse (array) form, `BLOCKSIZE` rows at a time, and fetched lazily with `sqlmore`.
  Every server cursor is released with `sqlclose`. Those closes are sent without waiting for the reply, so they
  never cost a round trip.
- **Parameter arrays** (`SQL_ATTR_PARAMSET_SIZE` > 1, e.g. pyodbc `fast_executemany`) are sent as a single batch
  request. `SQLRowCount` reports the total.
- **Transactions:** `SQLEndTran` sends `COMMIT` or `ROLLBACK`.
  - Autocommit is a property of the server-side JDBC connection. Changing `SQL_ATTR_AUTOCOMMIT` while connected
    re-sends `connect` with `auto commit=false`/`true`.
  - The connection must have no open cursors at that point, and server job state such as CL changes to the
    library list is reset.
  - Set autocommit before connecting (or use `AUTOCOMMIT=no`) to avoid that.
- **ODBC escape sequences** (`{fn ...}`, `{d '...'}`, `{call ...}`) are passed through unchanged. The server's
  JDBC driver (JTOpen) processes them.
- **Catalog functions** query the `SYSIBM` catalog views, as mapepire-jdbc does. The driver applies the result set
  column types that ODBC specifies, and rewrites `DATA_TYPE` values into ODBC type codes.

### Data types

| Db2 for i | ODBC SQL type | Notes |
|---|---|---|
| `CHAR`, `VARCHAR`, `CLOB` | `SQL_CHAR`, `SQL_VARCHAR`, `SQL_LONGVARCHAR` | |
| `GRAPHIC`/`NCHAR`, `VARGRAPHIC`/`NVARCHAR`, `DBCLOB` | `SQL_WCHAR`, `SQL_WVARCHAR`, `SQL_WLONGVARCHAR` | |
| `BINARY`, `VARBINARY`, `CHAR/VARCHAR FOR BIT DATA`, `BLOB`, `ROWID` | `SQL_BINARY`, `SQL_VARBINARY`, `SQL_LONGVARBINARY` | Binary parameters are sent as Base64. In WebSocket mode the real server returns LOBs as `blob_url` links, which the driver downloads over HTTPS. |
| `SMALLINT`, `INTEGER`, `BIGINT` | `SQL_SMALLINT`, `SQL_INTEGER`, `SQL_BIGINT` | `BIGINT` keeps full 64-bit precision. |
| `DECIMAL`, `NUMERIC` | `SQL_DECIMAL`, `SQL_NUMERIC` | Exact. Values are kept as decimal text, including exponent forms like `1E-7`. |
| `REAL`, `DOUBLE`, `FLOAT`, `DECFLOAT` | `SQL_REAL`, `SQL_DOUBLE` | Fetch `DECFLOAT` as `SQL_C_CHAR` to keep all 34 digits. |
| `DATE`, `TIME`, `TIMESTAMP` | `SQL_TYPE_DATE`, `SQL_TYPE_TIME`, `SQL_TYPE_TIMESTAMP` | Up to 12 fractional digits. Values are parsed from ISO or Db2 text formats. |
| `BOOLEAN` | `SQL_BIT` | |
| `XML`, `DATALINK`, others | `SQL_LONGVARCHAR` / `SQL_VARCHAR` | |

All standard C types are supported: character (`SQL_C_CHAR`/`SQL_C_WCHAR`), binary, every integer width, float and
double, bit, `SQL_C_NUMERIC`, and the date/time/timestamp structures.

## Limitations

- Cursors are forward-only and read-only. There is no `SQLSetPos`, `SQLBulkOperations`, bookmarks or scrollable
  cursors.
- One result set per statement: `SQLMoreResults` always returns `SQL_NO_DATA`. Stored procedures that return
  result sets only expose output parameters.
- There is no asynchronous execution (`SQL_ATTR_ASYNC_ENABLE`). `SQL_ATTR_QUERY_TIMEOUT` bounds how long the driver
  waits for each reply, but the protocol has no cancel request, so a timed-out statement keeps running on the
  server. `SQLCancel` only cancels pending data-at-execution.
- Descriptor APIs (`SQLGetDescField`, `SQLCopyDesc`, ...) and `SQLBrowseConnect` are not implemented. The Driver
  Manager reports them as unsupported.
- No connection dialog: `SQL_DRIVER_PROMPT` behaves like `SQL_DRIVER_NOPROMPT`.
- The real server decodes the Basic authentication header as Latin-1, so passwords outside Latin-1 cannot be used.
- Untested so far: the real Mapepire server, LOB downloads from real `blob_url` links (the mock has no LOB
  endpoint), and the OpenSSL TLS path of the socket transport. The Test workflow runs the Linux/OpenSSL path.

## Troubleshooting

Set the environment variable `MAPEPIRE_ODBC_TRACE` to a file path before starting the application. The driver then
appends every protocol message it sends and receives to that file, with a millisecond timestamp and thread id.
Passwords are never part of the protocol messages, but query text and result data are, so treat the file as
sensitive.

```powershell
$env:MAPEPIRE_ODBC_TRACE = "$env:TEMP\mapepire-odbc.log"
```

## Testing

`unit_tests` (built by CMake) covers JSON, Unicode, SHA-1/Base64, number normalisation, type mapping, connection
string parsing and parameter-marker counting.

The integration tests in [tests/integration](tests/integration) start the Mapepire mock server v0.2.0, using
[tests/integration/rules.json](tests/integration/rules.json) for extra scenarios. They then exercise the driver
through the platform's ODBC Driver Manager in two ways: with pyodbc, and by calling the ODBC API directly through
ctypes (block cursors, chunked `SQLGetData`, conversions, output parameters, data-at-execution, parameter arrays and
diagnostics).

```sh
pip install -r tests/requirements.txt
python -m pytest tests/integration            # needs Java 8+ for the mock server
```

The mock server jar is downloaded from its GitHub release the first time the tests run. Set `MAPEPIRE_MOCK_JAR` to
use a local jar, and `MAPEPIRE_ODBC_DRIVER` to test a driver binary other than `build/mapepireodbc.dll` /
`build/libmapepireodbc.so`. On Windows the tests create, and later remove, a temporary per-user DSN, so no
administrator rights are needed.

## License

Apache License 2.0. See [LICENSE](LICENSE).

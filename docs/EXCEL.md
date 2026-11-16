# Using the Mapepire ODBC driver with Excel

These steps use Excel's Power Query ODBC connector (Excel for Microsoft 365 / 2016 and later) against the
[Mapepire mock server](https://github.com/Mapepire-IBMi/mapepire-mockserver) v0.2.0. To use a real Mapepire server,
change the server, port and certificate settings in step 2.

The driver's bitness must match Excel's. 64-bit Excel is installed in `C:\Program Files\Microsoft Office`, so it
needs the x64 build `dist\windows-x64\mapepireodbc.dll`.

## 1. Start the mock server

```powershell
java -jar ..\mapepire-mockserver\target\mapepire-mockserver-0.2.0-jar-with-dependencies.jar --rules tests\integration\rules.json
```

The server listens on `wss://localhost:7892/db/` and accepts user `MOCK` with password `mockingbird`. It has two
sources of test data:

- **Built-in rules** (from `--rules` with `include_defaults`), for example `SELECT * FROM SAMPLE.DEPARTMENT`, which
  returns 14 rows.
- **The test suite's rules file**, which adds `SELECT * FROM ODBCTEST.TYPED` (one column of each common type:
  integers, a 64-bit BIGINT, DOUBLE, CHAR/VARCHAR with accents, DATE, TIME, TIMESTAMP, BOOLEAN and binary) and
  `SELECT * FROM ODBCTEST.PRECISE` (high-precision DECIMALs). These are useful for checking how Excel shows each
  type.

Any other SQL runs in the mock's in-memory SQLite database. For example, you can `CREATE SCHEMA DEMO` and then
`CREATE TABLE DEMO.T (...)` from another client, and query it from Excel.

## 2. Create a data source (no admin rights needed)

From the repository root:

```powershell
.\scripts\register-windows.ps1 -AddUserDsn -Name MapepireMock -DllPath .\dist\windows-x64\mapepireodbc.dll `
    -Server localhost -Port 7892 `
    -CertSHA256 '21:37:DA:8C:F3:76:37:BF:AF:B2:B1:4E:1E:F9:0B:AB:2A:1F:AE:A4:D5:06:67:85:3E:E0:4B:EF:7F:A0:A5:E4'
```

This creates a user DSN that points directly at the DLL. `CertSHA256` is the fingerprint of the mock server's
bundled self-signed certificate, so the connection is encrypted and the certificate is pinned.

The DSN appears in *ODBC Data Sources (64-bit)* under **User DSN**. The driver has no configuration dialog, so the
**Configure** button there does nothing useful; edit settings with the script instead. Remove the DSN with
`.\scripts\register-windows.ps1 -RemoveUserDsn -Name MapepireMock`.

## 3. Query from Excel

1. In Excel, open **Data > Get Data > From Other Sources > From ODBC**.
2. Under **Data source name (DSN)**, choose **MapepireMock**.
3. Expand **Advanced options** and enter a **SQL statement**, for example:
   ```sql
   SELECT * FROM SAMPLE.DEPARTMENT
   ```
   Against the mock, always enter a SQL statement (see the note below).
4. Click **OK**. When Excel asks for credentials, choose **Database** on the left, enter user name `MOCK` and
   password `mockingbird`, and click **Connect**. Excel passes them to the driver as `UID`/`PWD`.
5. In the preview, click **Load** to put the rows on a sheet, or **Transform Data** to open the Power Query editor.

**Navigator note:** without a SQL statement, Power Query opens its Navigator, which lists tables using
`SQLTables`/`SQLColumns`. The driver answers those from Db2 for i's `SYSIBM` catalog views. The mock server does
not provide those views, so the Navigator shows an error or nothing. On a real IBM i the Navigator works normally.

Afterwards:

- Refresh with **Data > Refresh All**.
- Change saved credentials in **Data > Get Data > Data Source Settings**: select the `dsn=MapepireMock` entry, then
  **Edit Permissions**.

## Things worth trying

- `SELECT * FROM ODBCTEST.TYPED`: check that dates are real Excel dates, that `9007199254740993` keeps all digits
  (Power Query shows it as a number; Excel itself rounds beyond 15 digits when displaying), that `héllo
  wörld ☃` keeps its accents and snowman, and how BOOLEAN and binary columns appear.
- `SELECT * FROM ODBCTEST.PRECISE`: high-precision DECIMALs, including `1E-7` from the server, which should become
  `0.0000001`.
- A bad password: Excel should report `[28000] ... The server refused the WebSocket connection`.
- A wrong fingerprint in the DSN: Excel should report a certificate mismatch, and the credentials are never sent.

## Troubleshooting

**Protocol trace.** Set `MAPEPIRE_ODBC_TRACE` to a file path before Excel starts. The variable must be in Excel's
environment, so start Excel from the same PowerShell window:

```powershell
$env:MAPEPIRE_ODBC_TRACE = "$env:TEMP\mapepire-odbc.log"
& "C:\Program Files\Microsoft Office\root\Office16\EXCEL.EXE"
```

Every request and reply is appended to the file, with a timestamp and thread id. Passwords are not in the trace,
but query text and data are.

**Replacing the DLL.** Excel keeps the DLL loaded while it runs. Close Excel before copying a new build over
`dist\windows-x64\mapepireodbc.dll`.

**"Timed out after 30000 ms waiting for the server" when connecting to the mock.** This is a known intermittent
issue that is still being investigated. It only shows up against the mock server over TLS: the reply to the first
request is sometimes delayed until the next request is sent. Retry the refresh.

**Driver errors** come back to Excel with an SQLSTATE and the prefix `[Mapepire][ODBC Driver]`. Errors from the
database add `[Db2 for i]` together with the SQL return code.

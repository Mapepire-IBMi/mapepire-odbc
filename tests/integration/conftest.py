"""
Integration test fixtures: start the Mapepire mock server (v0.2.0) and make the
freshly built driver reachable through the platform's ODBC Driver Manager.

Environment variables:
  MAPEPIRE_MOCK_JAR     path to mapepire-mockserver-*-jar-with-dependencies.jar
                        (default: download the v0.2.0 release jar into tests/.cache)
  MAPEPIRE_ODBC_DRIVER  path to the built driver (default: build/mapepireodbc.dll or
                        build/libmapepireodbc.so)
  JAVA                  java executable (default: java on PATH)
"""
import hashlib
import os
import socket
import ssl
import subprocess
import sys
import time
import urllib.request
import uuid
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
MOCK_VERSION = "0.2.0"
MOCK_URL = (
    "https://github.com/Mapepire-IBMi/mapepire-mockserver/releases/download/"
    f"v{MOCK_VERSION}/mapepire-mockserver-v{MOCK_VERSION}.jar"
)
MOCK_USER, MOCK_PASSWORD = "MOCK", "mockingbird"


def _driver_path() -> Path:
    env = os.environ.get("MAPEPIRE_ODBC_DRIVER")
    if env:
        return Path(env).resolve()
    name = "mapepireodbc.dll" if sys.platform == "win32" else "libmapepireodbc.so"
    return ROOT / "build" / name


def _mock_jar() -> Path:
    env = os.environ.get("MAPEPIRE_MOCK_JAR")
    if env:
        return Path(env).resolve()
    cache = ROOT / "tests" / ".cache"
    cache.mkdir(parents=True, exist_ok=True)
    jar = cache / f"mapepire-mockserver-v{MOCK_VERSION}.jar"
    if not jar.exists():
        tmp = jar.with_suffix(".part")
        urllib.request.urlretrieve(MOCK_URL, tmp)
        tmp.replace(jar)
    return jar


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class MockServer:
    def __init__(self, jar: Path, secure: bool):
        self.port = _free_port()
        self.secure = secure
        args = [os.environ.get("JAVA", "java"), "-jar", str(jar), "--port", str(self.port),
                "--host", "127.0.0.1", "--rules", str(HERE / "rules.json")]
        if not secure:
            args.append("--unsecure")
        log = os.environ.get("MAPEPIRE_MOCK_LOG")  # set to a directory to keep verbose server logs
        if log:
            args.append("--verbose")
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        self.log = open(os.path.join(log, f"mock-{self.port}.log"), "w") if log else None
        line = self.proc.stdout.readline()
        if "listening" not in line:
            self.proc.kill()
            raise RuntimeError(f"mock server failed to start: {line}{self.proc.stdout.read()}")
        # keep draining output so the server never blocks on a full pipe
        import threading
        threading.Thread(target=self._drain, daemon=True).start()
        self.fingerprint = self._fingerprint() if secure else None

    def _drain(self):
        for line in self.proc.stdout:
            if self.log:
                self.log.write(line)
                self.log.flush()

    def _fingerprint(self) -> str:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        for _ in range(50):
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=5) as raw:
                    with ctx.wrap_socket(raw, server_hostname="localhost") as tls:
                        der = tls.getpeercert(binary_form=True)
                        return hashlib.sha256(der).hexdigest()
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("could not read the mock server certificate")

    def stop(self):
        self.proc.kill()
        self.proc.wait(timeout=10)


@pytest.fixture(scope="session")
def driver_path() -> Path:
    p = _driver_path()
    if not p.exists():
        pytest.exit(f"driver not built: {p} (run cmake --build build first)", returncode=2)
    return p


@pytest.fixture(scope="session")
def mock_tls():
    srv = MockServer(_mock_jar(), secure=True)
    yield srv
    srv.stop()


@pytest.fixture(scope="session")
def mock_ws():
    srv = MockServer(_mock_jar(), secure=False)
    yield srv
    srv.stop()


@pytest.fixture(scope="session")
def driver_conn_part(driver_path):
    """
    Connection-string fragment that selects this driver build.

    unixODBC accepts DRIVER=<path>.  The Windows Driver Manager only accepts
    registered driver names, which needs admin rights, so on Windows a
    per-user DSN (HKCU) pointing at the DLL is created for the session.
    """
    if sys.platform != "win32":
        yield f"DRIVER={{{driver_path}}}"
        return
    import winreg
    dsn = f"MapepireOdbcTest_{uuid.uuid4().hex[:8]}"
    base = r"Software\ODBC\ODBC.INI"
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + "\\" + dsn) as k:
        winreg.SetValueEx(k, "Driver", 0, winreg.REG_SZ, str(driver_path))
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, base + r"\ODBC Data Sources") as k:
        winreg.SetValueEx(k, dsn, 0, winreg.REG_SZ, "Mapepire ODBC Driver (test build)")
    yield f"DSN={dsn}"
    winreg.DeleteKey(winreg.HKEY_CURRENT_USER, base + "\\" + dsn)
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, base + r"\ODBC Data Sources", 0, winreg.KEY_SET_VALUE) as k:
        winreg.DeleteValue(k, dsn)


@pytest.fixture(scope="session")
def connstr(driver_conn_part, mock_tls):
    """Build a connection string for the TLS mock (pinned certificate by default)."""
    def build(**overrides):
        attrs = {
            "SERVER": "127.0.0.1",
            "PORT": str(mock_tls.port),
            "UID": MOCK_USER,
            "PWD": MOCK_PASSWORD,
            "CERTSHA256": mock_tls.fingerprint,
        }
        attrs.update({k.upper(): v for k, v in overrides.items()})
        parts = [driver_conn_part] + [f"{k}={{{v}}}" for k, v in attrs.items() if v is not None]
        return ";".join(parts)

    return build


@pytest.fixture(scope="session")
def ws_connstr(driver_conn_part, mock_ws):
    def build(**overrides):
        attrs = {"SERVER": "127.0.0.1", "PORT": str(mock_ws.port), "UID": MOCK_USER, "PWD": MOCK_PASSWORD,
                 "SECURE": "no"}
        attrs.update({k.upper(): v for k, v in overrides.items()})
        return ";".join([driver_conn_part] + [f"{k}={{{v}}}" for k, v in attrs.items() if v is not None])

    return build


@pytest.fixture()
def unique():
    """A unique, valid Db2 identifier suffix (the mock's SQLite store is shared)."""
    return uuid.uuid4().hex[:10].upper()

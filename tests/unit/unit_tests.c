/*
 * Unit tests for the driver's self-contained building blocks.
 * Run: ./unit_tests   (exit code 0 = all passed)
 */
#include "drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0, checks = 0;

#define CHECK(_cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(_cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #_cond);      \
        }                                                                        \
    } while (0)

#define CHECK_STR(_a, _b)                                                                     \
    do {                                                                                    \
        checks++;                                                                           \
        const char *got_ = (_a), *want_ = (_b);                                                    \
        if (!got_ || strcmp(got_, want_)) {                                                        \
            failures++;                                                                     \
            fprintf(stderr, "FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__,    \
                    got_ ? got_ : "(null)", want_);                                                \
        }                                                                                   \
    } while (0)

static char *roundtrip(const char *_in)
{
    char err[128];
    mpjson *v = mpjson_parse(_in, strlen(_in), err, sizeof err);
    if (!v) return mapepire_strdup(err);
    mapepire_buf b;
    mapepire_buf_init(&b);
    mpjson_write(v, &b);
    mpjson_free(v);
    return mapepire_buf_detach(&b);
}

static void test_json(void)
{
    char *s;
    s = roundtrip("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\\"y\"}");
    CHECK_STR(s, "{\"a\":1,\"b\":[true,false,null],\"c\":\"x\\\"y\"}");
    free(s);
    /* number tokens are preserved exactly (DECIMAL precision) */
    s = roundtrip("[12345678901234567890.123456789, 1E-7, -0.50]");
    CHECK_STR(s, "[12345678901234567890.123456789,1E-7,-0.50]");
    free(s);
    /* unicode escapes, including a surrogate pair, become UTF-8 and are written back raw */
    s = roundtrip("\"\\u00e9\\ud83d\\ude00\"");
    CHECK_STR(s, "\"\xc3\xa9\xf0\x9f\x98\x80\"");
    free(s);
    /* control characters are escaped on output */
    s = roundtrip("\"a\\nb\\u0001\"");
    CHECK_STR(s, "\"a\\nb\\u0001\"");
    free(s);
    /* the real server terminates every frame with a newline */
    char err[64];
    mpjson *v = mpjson_parse("{\"id\":\"1\"}\n", 11, err, sizeof err);
    CHECK(v != NULL);
    CHECK_STR(mpjson_get_str(v, "id"), "1");
    mpjson_free(v);
    CHECK(mpjson_parse("{\"a\":}", 6, err, sizeof err) == NULL);
    CHECK(mpjson_parse("[1,]", 4, err, sizeof err) == NULL);
    CHECK(mpjson_parse("01", 2, err, sizeof err) == NULL);
    CHECK(mpjson_parse("\"abc", 4, err, sizeof err) == NULL);
    v = mpjson_parse("{\"n\":42,\"b\":true,\"s\":\"7\"}", 25, err, sizeof err);
    CHECK(mpjson_get_int(v, "n", 0) == 42);
    CHECK(mpjson_get_bool(v, "b", 0) == 1);
    CHECK(mpjson_get_int(v, "s", 0) == 7);
    CHECK(mpjson_get_int(v, "missing", -3) == -3);
    mpjson_free(v);
}

static void test_unicode(void)
{
    const char *u8 = "A\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"; /* A é € 😀 */
    uint16_t w[16];
    size_t n = mapepire_utf8_to_utf16(u8, strlen(u8), w, 16);
    CHECK(n == 5);
    CHECK(w[0] == 'A' && w[1] == 0xE9 && w[2] == 0x20AC && w[3] == 0xD83D && w[4] == 0xDE00 && w[5] == 0);
    char *back = mapepire_utf16_to_utf8(w, -1, NULL);
    CHECK_STR(back, u8);
    free(back);
    /* truncation never splits a surrogate pair */
    uint16_t small[5];
    n = mapepire_utf8_to_utf16(u8, strlen(u8), small, 5);
    CHECK(n == 5);
    CHECK(small[3] == 0);
    /* offsets for chunked SQLGetData */
    CHECK(mapepire_utf8_offset_for_utf16_units(u8, strlen(u8), 2) == 3);
    CHECK(mapepire_utf8_offset_for_utf16_units(u8, strlen(u8), 4) == 6); /* the pair does not fit */
    /* sanitizing keeps valid text and replaces invalid bytes (e.g. CP1252) */
    size_t sl;
    char *san = mapepire_utf8_sanitize("caf\xe9 \xc3\xa9", 7, &sl);
    CHECK_STR(san, "caf\xef\xbf\xbd \xc3\xa9");
    free(san);
    /* invalid UTF-8 becomes U+FFFD */
    n = mapepire_utf8_to_utf16("\xff", 1, w, 16);
    CHECK(n == 1 && w[0] == 0xFFFD);
}

static void test_crypto(void)
{
    uint8_t d[20];
    mapepire_sha1("abc", 3, d);
    static const uint8_t want[20] = {0xa9, 0x99, 0x3e, 0x36, 0x47, 0x06, 0x81, 0x6a, 0xba, 0x3e,
                                     0x25, 0x71, 0x78, 0x50, 0xc2, 0x6c, 0x9c, 0xd0, 0xd8, 0x9d};
    CHECK(!memcmp(d, want, 20));
    /* RFC 6455 section 1.3 example */
    const char *key = "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    mapepire_sha1(key, strlen(key), d);
    char *acc = mapepire_base64_encode(d, 20);
    CHECK_STR(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    free(acc);
    char *b = mapepire_base64_encode("MOCK:mockingbird", 16);
    CHECK_STR(b, "TU9DSzptb2NraW5nYmlyZA==");
    free(b);
    char *auth = mapepire_ws_basic_auth("MOCK", "mockingbird");
    CHECK_STR(auth, "Basic TU9DSzptb2NraW5nYmlyZA==");
    free(auth);
    char fp[65];
    CHECK(mapepire_ws_normalize_fingerprint("21:37:DA:8C:F3:76:37:BF:AF:B2:B1:4E:1E:F9:0B:AB:2A:1F:AE:A4:D5:06:67:85:3E:E0:4B:EF:7F:A0:A5:E4", fp) == 0);
    CHECK_STR(fp, "2137da8cf37637bfafb2b14e1ef90bab2a1faea4d50667853ee04bef7fa0a5e4");
    CHECK(mapepire_ws_normalize_fingerprint("abc", fp) != 0);
}

static void test_numbers(void)
{
    char out[128];
    CHECK(mapepire_number_normalize("1E-7", out, sizeof out) == 0);
    CHECK_STR(out, "0.0000001");
    CHECK(mapepire_number_normalize("1.25E+3", out, sizeof out) == 0);
    CHECK_STR(out, "1250");
    CHECK(mapepire_number_normalize("-1.2345E2", out, sizeof out) == 0);
    CHECK_STR(out, "-123.45");
    CHECK(mapepire_number_normalize("  +007.50 ", out, sizeof out) == 0);
    CHECK_STR(out, "7.50");
    CHECK(mapepire_number_normalize("0.001", out, sizeof out) == 0);
    CHECK_STR(out, "0.001");
    CHECK(mapepire_number_normalize(".5", out, sizeof out) == 0);
    CHECK_STR(out, "0.5");
    CHECK(mapepire_number_normalize("12345678901234567890123456789", out, sizeof out) == 0);
    CHECK_STR(out, "12345678901234567890123456789");
    CHECK(mapepire_number_normalize("abc", out, sizeof out) != 0);
    CHECK(mapepire_number_normalize("1e", out, sizeof out) != 0);
    CHECK(mapepire_number_normalize("", out, sizeof out) != 0);
}

static void test_types(void)
{
    mapepire_col c;
    memset(&c, 0, sizeof c);
    CHECK(mapepire_type_describe("DECIMAL", 11, 2, &c) && c.sql_type == SQL_DECIMAL && c.size == 11 && c.digits == 2);
    CHECK(mapepire_type_describe("CHAR () FOR BIT DATA", 16, 0, &c) && c.sql_type == SQL_BINARY && c.size == 16);
    CHECK(mapepire_type_describe("VARCHAR () FOR BIT DATA", 100, 0, &c) && c.sql_type == SQL_VARBINARY);
    CHECK(mapepire_type_describe("TIMESTAMP", 26, 6, &c) && c.sql_type == SQL_TYPE_TIMESTAMP && c.size == 26 && c.digits == 6);
    CHECK(mapepire_type_describe("timestamp", 0, 0, &c) && c.size == 26);
    CHECK(mapepire_type_describe("NVARCHAR", 20, 0, &c) && c.sql_type == SQL_WVARCHAR);
    CHECK(mapepire_type_describe("BOOLEAN", 1, 0, &c) && c.sql_type == SQL_BIT);
    CHECK(mapepire_type_describe("FLOAT", 53, 0, &c) && c.sql_type == SQL_DOUBLE);
    CHECK(mapepire_type_describe("FLOAT", 24, 0, &c) && c.sql_type == SQL_REAL);
    CHECK(mapepire_type_describe("CHARACTER", 10, 0, &c) && c.sql_type == SQL_CHAR);
    CHECK(!mapepire_type_describe("ARRAY", 0, 0, &c) && c.sql_type == SQL_VARCHAR);
}

static void test_conf(void)
{
    mapepire_conf c;
    memset(&c, 0, sizeof c);
    char err[128];
    CHECK(mapepire_conf_parse(&c, "Driver={Mapepire ODBC Driver};host=h1;Port=7892;User=mock;"
                            "Password={a;b}}c};Props={naming=system;libraries=MYLIB}; trustcert = yes ;",
                        err, sizeof err) == 0);
    CHECK_STR(mapepire_conf_get(&c, "DRIVER"), "Mapepire ODBC Driver");
    CHECK_STR(mapepire_conf_get(&c, "SERVER"), "h1");
    CHECK_STR(mapepire_conf_get(&c, "UID"), "mock");
    CHECK_STR(mapepire_conf_get(&c, "PWD"), "a;b}c");
    CHECK_STR(mapepire_conf_get(&c, "PROPS"), "naming=system;libraries=MYLIB");
    CHECK_STR(mapepire_conf_get(&c, "TRUSTSERVERCERTIFICATE"), "yes");
    char *out = mapepire_conf_to_string(&c);
    mapepire_conf c2;
    memset(&c2, 0, sizeof c2);
    CHECK(mapepire_conf_parse(&c2, out, err, sizeof err) == 0);
    CHECK_STR(mapepire_conf_get(&c2, "PWD"), "a;b}c");
    CHECK_STR(mapepire_conf_get(&c2, "PROPS"), "naming=system;libraries=MYLIB");
    free(out);
    mapepire_conf_free(&c2);
    CHECK(mapepire_conf_parse(&c, "SERVER={unterminated", err, sizeof err) != 0);
    CHECK(mapepire_conf_parse(&c, "novalue", err, sizeof err) != 0);
    mapepire_conf_free(&c);
}

static void test_markers(void)
{
    CHECK(mapepire_count_markers("SELECT * FROM T WHERE A = ? AND B = ?") == 2);
    CHECK(mapepire_count_markers("SELECT '?', \"a?b\" FROM T WHERE X = ?") == 1);
    CHECK(mapepire_count_markers("SELECT 'it''s ?' FROM T -- ? comment\n WHERE A = ?") == 1);
    CHECK(mapepire_count_markers("/* ? */ VALUES ?") == 1);
    CHECK(mapepire_count_markers("CALL P(?, ?, ?)") == 3);
    CHECK(mapepire_count_markers("") == 0);
}

int main(void)
{
    test_json();
    test_unicode();
    test_crypto();
    test_numbers();
    test_types();
    test_conf();
    test_markers();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

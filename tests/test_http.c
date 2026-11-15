/*
 * Unit tests for the HTTP parser and path helpers.   make unit
 */
#include "http.h"
#include "mime.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        const char *a_ = (actual), *e_ = (expected);                         \
        checks++;                                                            \
        if (!a_ || strcmp(a_, e_) != 0) {                                    \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: expected \"%s\", got \"%s\"\n",          \
                    __FILE__, __LINE__, e_, a_ ? a_ : "(null)");             \
        }                                                                    \
    } while (0)

static char buf[HTTP_MAX_REQUEST];

/* Parse from a string literal (copied first: the parser writes into its input). */
static http_parse_result_t parse(const char *raw, http_request_t *req)
{
    size_t len = strlen(raw);
    memcpy(buf, raw, len);
    return http_parse_request(buf, len, req);
}

static void test_parse_valid(void)
{
    http_request_t r;

    CHECK(parse("GET /index.html?x=1 HTTP/1.1\r\n"
                "Host: localhost:8080\r\n"
                "User-Agent:   curl/8.5.0  \r\n"
                "Accept: */*\r\n"
                "\r\n", &r) == HTTP_PARSE_OK);
    CHECK(r.method == HTTP_GET);
    CHECK_STR(r.method_name, "GET");
    CHECK_STR(r.target, "/index.html?x=1");
    CHECK(r.version_minor == 1);
    CHECK(r.num_headers == 3);
    CHECK_STR(http_get_header(&r, "host"), "localhost:8080");
    CHECK_STR(http_get_header(&r, "USER-AGENT"), "curl/8.5.0");
    CHECK(http_get_header(&r, "Cookie") == NULL);

    CHECK(parse("HEAD / HTTP/1.1\r\nHost: x\r\n\r\n", &r) == HTTP_PARSE_OK);
    CHECK(r.method == HTTP_HEAD);

    CHECK(parse("DELETE /a HTTP/1.1\r\nHost: x\r\n\r\n", &r) == HTTP_PARSE_OK);
    CHECK(r.method == HTTP_OTHER);

    CHECK(parse("GET / HTTP/1.0\r\n\r\n", &r) == HTTP_PARSE_OK);   /* 1.0: Host optional */
    CHECK(r.version_minor == 0);
    CHECK(r.num_headers == 0);

    CHECK(parse("GET / HTTP/1.1\r\nHost: x\r\nX-Empty:\r\n\r\n", &r) == HTTP_PARSE_OK);
    CHECK_STR(http_get_header(&r, "x-empty"), "");
}

static void test_parse_invalid(void)
{
    static const char *const bad[] = {
        "GET /\r\n\r\n",                                     /* no version */
        "GET  / HTTP/1.1\r\nHost: x\r\n\r\n",                /* double space */
        "GET / HTTP/1.1 \r\nHost: x\r\n\r\n",                /* trailing space */
        "GET index.html HTTP/1.1\r\nHost: x\r\n\r\n",        /* not origin-form */
        "GET /a\x7f HTTP/1.1\r\nHost: x\r\n\r\n",            /* DEL in target */
        "G(T / HTTP/1.1\r\nHost: x\r\n\r\n",                 /* bad method char */
        "GET / HTTX/1.1\r\nHost: x\r\n\r\n",
        "GET / HTTP/1\r\nHost: x\r\n\r\n",
        "GET / HTTP/1.1\r\n\r\n",                            /* 1.1 without Host */
        "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",      /* two Hosts */
        "GET / HTTP/1.1\r\nHost: x\r\nno colon\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : x\r\n\r\n",                /* space before colon */
        "GET / HTTP/1.1\r\nHost: x\r\n folded: y\r\n\r\n",   /* obsolete folding */
        "GET / HTTP/1.1\r\nHost: x\r\nX-A: a\x01z\r\n\r\n",  /* control char */
        "GET / HTTP/1.1\nHost: x\n\n",                       /* bare LF */
    };
    http_request_t r;

    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        checks++;
        if (parse(bad[i], &r) != HTTP_PARSE_BAD_REQUEST) {
            failures++;
            fprintf(stderr, "%s:%d: accepted bad request #%zu\n", __FILE__, __LINE__, i);
        }
    }

    static const char with_nul[] = "GET /a\0b HTTP/1.1\r\nHost: x\r\n\r\n";
    memcpy(buf, with_nul, sizeof with_nul - 1);
    CHECK(http_parse_request(buf, sizeof with_nul - 1, &r) == HTTP_PARSE_BAD_REQUEST);

    /* One header too many. */
    char many[HTTP_MAX_REQUEST];
    size_t n = (size_t)sprintf(many, "GET / HTTP/1.1\r\nHost: x\r\n");
    for (int i = 0; i < HTTP_MAX_HEADERS; i++)
        n += (size_t)sprintf(many + n, "X-%d: y\r\n", i);
    strcpy(many + n, "\r\n");
    CHECK(parse(many, &r) == HTTP_PARSE_BAD_REQUEST);

    CHECK(parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n", &r) == HTTP_PARSE_BAD_VERSION);
    CHECK(parse("GET / HTTP/0.9\r\nHost: x\r\n\r\n", &r) == HTTP_PARSE_BAD_VERSION);
}

static void test_connection_headers(void)
{
    http_request_t r;

    parse("GET / HTTP/1.1\r\nHost: x\r\n\r\n", &r);
    CHECK(http_wants_keep_alive(&r));
    parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n", &r);
    CHECK(!http_wants_keep_alive(&r));
    parse("GET / HTTP/1.0\r\n\r\n", &r);
    CHECK(!http_wants_keep_alive(&r));
    parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &r);
    CHECK(http_wants_keep_alive(&r));

    parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\n", &r);
    CHECK(http_has_body(&r));
    parse("POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n", &r);
    CHECK(http_has_body(&r));
    parse("GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", &r);
    CHECK(!http_has_body(&r));

    CHECK(http_has_token("keep-alive, Upgrade", "upgrade"));
    CHECK(http_has_token("  close ", "close"));
    CHECK(!http_has_token("closed", "close"));
    CHECK(!http_has_token("", "close"));
}

static void test_decode_path(void)
{
    char out[64];

    CHECK(http_decode_path("/a%20b.html?q=%zz#frag", out, sizeof out) == 0);
    CHECK_STR(out, "/a b.html");
    CHECK(http_decode_path("/%E2%9c%93", out, sizeof out) == 0);
    CHECK_STR(out, "/\xE2\x9C\x93");
    CHECK(http_decode_path("/a+b", out, sizeof out) == 0);
    CHECK_STR(out, "/a+b");
    CHECK(http_decode_path("/%2e%2E/etc", out, sizeof out) == 0);   /* caught by normalize */
    CHECK_STR(out, "/../etc");

    CHECK(http_decode_path("/%00", out, sizeof out) == -1);
    CHECK(http_decode_path("/%0d%0a", out, sizeof out) == -1);
    CHECK(http_decode_path("/%zz", out, sizeof out) == -1);
    CHECK(http_decode_path("/%4", out, sizeof out) == -1);
    CHECK(http_decode_path("/%", out, sizeof out) == -1);
    CHECK(http_decode_path("/abcdefgh", out, 5) == -2);
}

static void test_normalize_path(void)
{
    static const struct { const char *in, *out; } cases[] = {
        { "/",               "/" },
        { "/index.html",     "/index.html" },
        { "//a///b",         "/a/b" },
        { "/a/b/",           "/a/b/" },
        { "/a/./b",          "/a/b" },
        { "/a/b/../c",       "/a/c" },
        { "/a/..",           "/" },
        { "/a/b/..",         "/a/" },
        { "/a/.",            "/a/" },
        { "/./",             "/" },
        { "/..a/b../.c",     "/..a/b../.c" },
    };
    char out[64];

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        CHECK(http_normalize_path(cases[i].in, out, sizeof out) == 0);
        CHECK_STR(out, cases[i].out);
    }
    CHECK(http_normalize_path("/..", out, sizeof out) == -1);
    CHECK(http_normalize_path("/a/../..", out, sizeof out) == -1);
    CHECK(http_normalize_path("/a/../../etc/passwd", out, sizeof out) == -1);
    CHECK(http_normalize_path("relative", out, sizeof out) == -1);
    CHECK(http_normalize_path("/abcdef", out, 5) == -2);
}

static void test_mime(void)
{
    CHECK_STR(mime_type("/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type("/INDEX.HTM"), "text/html; charset=utf-8");
    CHECK_STR(mime_type("/css/style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type("/logo.svg"), "image/svg+xml");
    CHECK_STR(mime_type("/Makefile"), "application/octet-stream");
    CHECK_STR(mime_type("/v1.2/README"), "application/octet-stream");
}

int main(void)
{
    test_parse_valid();
    test_parse_invalid();
    test_connection_headers();
    test_decode_path();
    test_normalize_path();
    test_mime();

    printf("unit tests: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}

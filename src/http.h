#ifndef MINIHTTP_HTTP_H
#define MINIHTTP_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define SERVER_NAME       "minihttp/1.0"
#define HTTP_MAX_REQUEST  8192   /* request line + all headers */
#define HTTP_MAX_HEADERS  32

typedef enum { HTTP_GET, HTTP_HEAD, HTTP_OTHER } http_method_t;

typedef struct {
    const char *name;
    const char *value;
} http_header_t;

/* A parsed request. Every string points into the caller's buffer. */
typedef struct {
    http_method_t method;
    const char   *method_name;    /* as sent, e.g. "GET" */
    const char   *target;         /* raw request-target, e.g. "/a%20b.html?x=1" */
    int           version_minor;  /* HTTP/1.<minor>: 0 or 1 */
    http_header_t headers[HTTP_MAX_HEADERS];
    size_t        num_headers;
} http_request_t;

typedef enum {
    HTTP_PARSE_OK,
    HTTP_PARSE_BAD_REQUEST,   /* -> 400 */
    HTTP_PARSE_BAD_VERSION,   /* -> 505 */
} http_parse_result_t;

/* ---- requests ---------------------------------------------------------- */

/*
 * Parse a request head: request line, headers and the terminating empty
 * line, `len` bytes in total. The buffer is modified in place (NULs are
 * written after each token) and must outlive `req`.
 */
http_parse_result_t http_parse_request(char *buf, size_t len, http_request_t *req);

/* Case-insensitive header lookup; NULL if absent. */
const char *http_get_header(const http_request_t *req, const char *name);

/* True if a comma-separated header value lists `token` (case-insensitive). */
bool http_has_token(const char *value, const char *token);

/* HTTP/1.1 defaults to keep-alive, HTTP/1.0 to close. */
bool http_wants_keep_alive(const http_request_t *req);

/* True if the request announces a message body. */
bool http_has_body(const http_request_t *req);

/*
 * Percent-decode the path part of a request-target (query and fragment are
 * dropped). Returns 0, -1 for malformed input or control characters, or -2
 * if the result does not fit in `size` bytes.
 */
int http_decode_path(const char *target, char *out, size_t size);

/*
 * Resolve ".", ".." and repeated slashes in an absolute URL path, keeping a
 * trailing slash. Returns 0, -1 if the path climbs above "/", or -2 if the
 * result does not fit in `size` bytes.
 */
int http_normalize_path(const char *path, char *out, size_t size);

/* ---- responses --------------------------------------------------------- */

const char *http_status_text(int status);

/* RFC 9110 date, e.g. "Sun, 06 Nov 1994 08:49:37 GMT". */
void http_format_date(time_t t, char *out, size_t size);

/* send() until everything is written. Returns 0 or -1. */
int http_send_all(int fd, const void *buf, size_t len, int flags);

/*
 * Write the status line and headers. `extra_headers` holds additional
 * CRLF-terminated header lines, or NULL. Pass body_follows=true when the
 * body is sent by a separate call, so the kernel can put both in one packet.
 */
int http_send_head(int fd, int status, const char *content_type,
                   long long content_length, bool keep_alive,
                   const char *extra_headers, bool body_follows);

/* Send a complete response with a small HTML body describing `status`.
 * Returns the number of body bytes written, or -1 on error. */
long long http_send_error(int fd, int status, bool keep_alive, bool head_only,
                          const char *extra_headers);

#endif

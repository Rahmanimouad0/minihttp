/*
 * http.c — HTTP/1.1 request parsing and response writing.
 *
 * The parser is deliberately strict: it accepts what RFC 9112 allows for a
 * request head and rejects everything else with 400. Being picky here keeps
 * the rest of the server simple and hard to confuse.
 */
#include "http.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

/* ---- request parsing --------------------------------------------------- */

/* RFC 9110 "tchar": the characters allowed in methods and header names. */
static bool is_tchar(unsigned char c)
{
    return isalnum(c) || (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static bool is_token(const char *s)
{
    if (*s == '\0')
        return false;
    for (; *s; s++)
        if (!is_tchar((unsigned char)*s))
            return false;
    return true;
}

/* Origin-form ("/path?query") made of visible ASCII only. Rejecting spaces,
 * control characters and raw 8-bit bytes here means the target is always
 * safe to print in logs or echo back in a header. */
static bool is_valid_target(const char *s)
{
    if (s[0] != '/')
        return false;
    for (; *s; s++)
        if ((unsigned char)*s <= 0x20 || (unsigned char)*s >= 0x7f)
            return false;
    return true;
}

static bool is_valid_field_value(const char *s)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && c != '\t') || c == 0x7f)
            return false;
    }
    return true;
}

/* Next CRLF in [p, end), or NULL. */
static char *find_crlf(char *p, const char *end)
{
    for (; p + 1 < end; p++)
        if (p[0] == '\r' && p[1] == '\n')
            return p;
    return NULL;
}

/* Strip spaces and tabs from both ends, in place. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        *--end = '\0';
    return s;
}

/* method SP request-target SP HTTP-version */
static http_parse_result_t parse_request_line(char *line, http_request_t *req)
{
    char *sp1 = strchr(line, ' ');
    if (!sp1)
        return HTTP_PARSE_BAD_REQUEST;
    *sp1 = '\0';

    char *target = sp1 + 1;
    char *sp2 = strchr(target, ' ');
    if (!sp2)
        return HTTP_PARSE_BAD_REQUEST;
    *sp2 = '\0';

    const char *version = sp2 + 1;
    if (!is_token(line) || !is_valid_target(target))
        return HTTP_PARSE_BAD_REQUEST;

    /* "HTTP/" DIGIT "." DIGIT */
    if (strncmp(version, "HTTP/", 5) != 0 ||
        !isdigit((unsigned char)version[5]) || version[6] != '.' ||
        !isdigit((unsigned char)version[7]) || version[8] != '\0')
        return HTTP_PARSE_BAD_REQUEST;
    if (version[5] != '1')
        return HTTP_PARSE_BAD_VERSION;

    req->method_name = line;
    req->method = strcmp(line, "GET") == 0  ? HTTP_GET
                : strcmp(line, "HEAD") == 0 ? HTTP_HEAD
                                            : HTTP_OTHER;
    req->target = target;
    req->version_minor = version[7] == '0' ? 0 : 1;   /* 1.x (x > 1) acts as 1.1 */
    return HTTP_PARSE_OK;
}

http_parse_result_t http_parse_request(char *buf, size_t len, http_request_t *req)
{
    memset(req, 0, sizeof *req);
    const char *end = buf + len;

    /* We NUL-terminate tokens in place, so a NUL in the input would
     * silently cut a token short. */
    if (memchr(buf, '\0', len))
        return HTTP_PARSE_BAD_REQUEST;

    char *eol = find_crlf(buf, end);
    if (!eol)
        return HTTP_PARSE_BAD_REQUEST;
    *eol = '\0';
    http_parse_result_t result = parse_request_line(buf, req);
    if (result != HTTP_PARSE_OK)
        return result;

    int hosts = 0;
    for (char *line = eol + 2;; line = eol + 2) {
        eol = find_crlf(line, end);
        if (!eol)
            return HTTP_PARSE_BAD_REQUEST;
        if (eol == line)
            break;                              /* empty line: end of head */
        *eol = '\0';

        char *colon = strchr(line, ':');
        if (!colon)
            return HTTP_PARSE_BAD_REQUEST;
        *colon = '\0';
        /* is_token() also rejects "Name :" and obsolete line folding
         * (continuation lines that start with whitespace). */
        if (!is_token(line))
            return HTTP_PARSE_BAD_REQUEST;
        char *value = trim(colon + 1);
        if (!is_valid_field_value(value))
            return HTTP_PARSE_BAD_REQUEST;
        if (req->num_headers == HTTP_MAX_HEADERS)
            return HTTP_PARSE_BAD_REQUEST;

        if (strcasecmp(line, "Host") == 0)
            hosts++;
        req->headers[req->num_headers++] = (http_header_t){ line, value };
    }

    /* HTTP/1.1 requires exactly one Host header (RFC 9112 §3.2). */
    if (hosts > 1 || (req->version_minor == 1 && hosts == 0))
        return HTTP_PARSE_BAD_REQUEST;
    return HTTP_PARSE_OK;
}

const char *http_get_header(const http_request_t *req, const char *name)
{
    for (size_t i = 0; i < req->num_headers; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

bool http_has_token(const char *value, const char *token)
{
    size_t token_len = strlen(token);
    const char *p = value;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        const char *start = p;
        while (*p && *p != ',')
            p++;
        const char *stop = p;
        while (stop > start && (stop[-1] == ' ' || stop[-1] == '\t'))
            stop--;
        if ((size_t)(stop - start) == token_len && strncasecmp(start, token, token_len) == 0)
            return true;
    }
    return false;
}

bool http_wants_keep_alive(const http_request_t *req)
{
    const char *conn = http_get_header(req, "Connection");
    if (req->version_minor == 0)
        return conn && http_has_token(conn, "keep-alive");
    return !(conn && http_has_token(conn, "close"));
}

bool http_has_body(const http_request_t *req)
{
    const char *length = http_get_header(req, "Content-Length");
    return http_get_header(req, "Transfer-Encoding") != NULL ||
           (length && strcmp(length, "0") != 0);
}

static int hex_value(unsigned char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = (unsigned char)tolower(c);
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

int http_decode_path(const char *target, char *out, size_t size)
{
    size_t n = 0;

    for (const char *p = target; *p && *p != '?' && *p != '#'; p++) {
        int c = (unsigned char)*p;
        if (c == '%') {
            int hi = hex_value((unsigned char)p[1]);
            int lo = hi < 0 ? -1 : hex_value((unsigned char)p[2]);
            if (hi < 0 || lo < 0)
                return -1;
            c = hi * 16 + lo;
            p += 2;
        }
        /* NUL and control characters never belong in a file name we serve,
         * and would be dangerous in logs, headers or C strings. */
        if (c < 0x20 || c == 0x7f)
            return -1;
        if (n + 1 >= size)
            return -2;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return 0;
}

int http_normalize_path(const char *path, char *out, size_t size)
{
    if (path[0] != '/' || size < 2)
        return -1;

    /* `out` is built as "/seg/seg/" (always ending in '/') and the final
     * slash is dropped at the end unless the input named a directory. */
    size_t len = 1;
    out[0] = '/';
    bool trailing_slash = false;

    for (const char *p = path; *p;) {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *seg = p;
        while (*p && *p != '/')
            p++;
        size_t seg_len = (size_t)(p - seg);

        if (seg_len == 1 && seg[0] == '.') {
            trailing_slash = true;
        } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            if (len == 1)
                return -1;                   /* would climb above the root */
            len--;                           /* drop the slash after the last segment */
            while (out[len - 1] != '/')      /* ...and the segment itself */
                len--;
            trailing_slash = true;
        } else {
            if (len + seg_len + 2 > size)    /* segment + '/' + NUL */
                return -2;
            memcpy(out + len, seg, seg_len);
            len += seg_len;
            out[len++] = '/';
            trailing_slash = (*p == '/');
        }
    }

    if (!trailing_slash && len > 1)
        len--;
    out[len] = '\0';
    return 0;
}

/* ---- responses --------------------------------------------------------- */

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 301: return "Moved Permanently";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 414: return "URI Too Long";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

void http_format_date(time_t t, char *out, size_t size)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    /* We never call setlocale(), so day and month names are always English. */
    strftime(out, size, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

int http_send_all(int fd, const void *buf, size_t len, int flags)
{
    const char *p = buf;

    while (len > 0) {
        /* MSG_NOSIGNAL: a vanished peer gives EPIPE instead of SIGPIPE. */
        ssize_t n = send(fd, p, len, flags | MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int http_send_head(int fd, int status, const char *content_type,
                   long long content_length, bool keep_alive,
                   const char *extra_headers, bool body_follows)
{
    char date[64];
    char head[HTTP_MAX_REQUEST + 512];   /* extra headers may echo the target */

    http_format_date(time(NULL), date, sizeof date);
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Server: " SERVER_NAME "\r\n"
                     "Date: %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lld\r\n"
                     "Connection: %s\r\n"
                     "%s"
                     "\r\n",
                     status, http_status_text(status), date, content_type,
                     content_length, keep_alive ? "keep-alive" : "close",
                     extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= sizeof head)
        return -1;

    /* MSG_MORE holds the headers back until the body arrives, so a small
     * response leaves as one TCP segment instead of two. */
    return http_send_all(fd, head, (size_t)n, body_follows ? MSG_MORE : 0);
}

long long http_send_error(int fd, int status, bool keep_alive, bool head_only,
                          const char *extra_headers)
{
    char body[512];
    const char *text = http_status_text(status);
    int len = snprintf(body, sizeof body,
                       "<!DOCTYPE html>\n"
                       "<html><head><title>%d %s</title></head>\n"
                       "<body><h1>%d %s</h1><hr><p>" SERVER_NAME "</p></body></html>\n",
                       status, text, status, text);

    if (http_send_head(fd, status, "text/html; charset=utf-8", len, keep_alive,
                       extra_headers, !head_only) < 0)
        return -1;
    if (head_only)
        return 0;
    return http_send_all(fd, body, (size_t)len, 0) < 0 ? -1 : len;
}

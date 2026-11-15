/*
 * log.c — tiny thread-safe logger.
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void vlog(FILE *stream, const char *fmt, va_list ap)
{
    char stamp[32];
    char msg[2048];
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    vsnprintf(msg, sizeof msg, fmt, ap);
    /* stdio locks the stream for each call, so one fprintf() per line
     * keeps lines from different threads from interleaving. */
    fprintf(stream, "[%s] %s\n", stamp, msg);
}

void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stdout, fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog(stderr, fmt, ap);
    va_end(ap);
}

void log_access(const char *client_ip, const http_request_t *req, int status, long long bytes)
{
    char size[24] = "-";
    if (bytes >= 0)
        snprintf(size, sizeof size, "%lld", bytes);

    /* The target is validated printable ASCII, so it is safe to log as-is. */
    if (req)
        log_info("%s \"%s %s HTTP/1.%d\" %d %s", client_ip, req->method_name,
                 req->target, req->version_minor, status, size);
    else
        log_info("%s \"-\" %d %s", client_ip, status, size);
}

#ifndef MINIHTTP_LOG_H
#define MINIHTTP_LOG_H

#include "http.h"

/* Timestamped lines; info goes to stdout, errors to stderr. Thread-safe. */
void log_info(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* One access-log line per response. `req` is NULL when the request could
 * not be parsed; `bytes` < 0 means the response could not be written. */
void log_access(const char *client_ip, const http_request_t *req, int status, long long bytes);

#endif

#ifndef MINIHTTP_HANDLER_H
#define MINIHTTP_HANDLER_H

#include "http.h"

typedef struct {
    int       status;      /* status code sent to the client */
    long long bytes;       /* body bytes written, or -1 if writing failed */
    bool      keep_alive;  /* whether the connection may serve another request */
} http_response_t;

/* Serve one request from the document root `root` (canonical, absolute)
 * and write the response to the socket `fd`. */
http_response_t handler_serve(const char *root, int fd, const http_request_t *req, bool keep_alive);

#endif

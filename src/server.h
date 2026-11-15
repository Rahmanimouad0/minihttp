#ifndef MINIHTTP_SERVER_H
#define MINIHTTP_SERVER_H

#include <stdint.h>

typedef struct {
    uint16_t    port;
    const char *root;         /* canonical absolute path of the document root */
    int         max_clients;  /* limit on concurrent connections */
} server_config_t;

/* Serve until SIGINT or SIGTERM, then shut down gracefully.
 * Returns 0 after a clean shutdown, -1 if the server could not start.
 * `cfg` must stay valid until the process exits. */
int server_run(const server_config_t *cfg);

#endif

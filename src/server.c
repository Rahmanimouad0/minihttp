/*
 * server.c — sockets, connection threads and graceful shutdown.
 *
 *   socket() → bind() → listen() → poll() → accept4() → one thread per client
 *
 * SIGINT and SIGTERM are blocked in every thread and read from a signalfd,
 * so the accept loop waits for "new connection" and "time to stop" with a
 * single poll(), and there are no asynchronous signal handlers at all.
 */
#include "server.h"
#include "handler.h"
#include "http.h"
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_BACKLOG      128
#define IDLE_TIMEOUT_SEC    5      /* max wait for (the rest of) a request */
#define SEND_TIMEOUT_SEC    30     /* max time one blocked send() may take */
#define LINGER_TIMEOUT_SEC  1      /* max time spent draining input before close */
#define SHUTDOWN_GRACE_SEC  10     /* max time to wait for in-flight responses */
#define THREAD_STACK_SIZE   (256 * 1024)

typedef struct {
    const server_config_t *cfg;
    pthread_attr_t  thread_attr;   /* detached, small stack */
    pthread_mutex_t lock;          /* guards slots[] and active */
    pthread_cond_t  drained;       /* signalled when active drops to zero */
    int            *slots;         /* slot -> client fd, or -1 when free */
    int             active;
    atomic_bool     stopping;
} server_t;

typedef struct {
    server_t *srv;
    int       fd;
    int       slot;
    char      ip[INET_ADDRSTRLEN];
} client_t;

/* ---- setup ------------------------------------------------------------- */

/* Route SIGINT/SIGTERM to a file descriptor. Threads inherit the signal
 * mask, so this must run before any thread is created. */
static int setup_signals(void)
{
    /* A client that disconnects mid-response must cost us an EPIPE,
     * not the whole process. */
    signal(SIGPIPE, SIG_IGN);

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    int err = pthread_sigmask(SIG_BLOCK, &mask, NULL);
    if (err != 0) {
        log_error("pthread_sigmask: %s", strerror(err));
        return -1;
    }

    int fd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (fd < 0)
        log_error("signalfd: %s", strerror(errno));
    return fd;
}

static int create_listener(uint16_t port)
{
    /* Non-blocking, so accept4() can never stall the loop if a client
     * resets between poll() and accept4(). */
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }

    /* Allow an immediate restart while old connections sit in TIME_WAIT. */
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
        log_error("setsockopt(SO_REUSEADDR): %s", strerror(errno));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        log_error("bind to port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        log_error("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void configure_client_socket(int fd)
{
    /* Timeouts bound how long a slow or silent client can pin a thread. */
    struct timeval recv_timeout = { .tv_sec = IDLE_TIMEOUT_SEC };
    struct timeval send_timeout = { .tv_sec = SEND_TIMEOUT_SEC };
    int one = 1;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof recv_timeout);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof send_timeout);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

/* ---- client registry --------------------------------------------------- */

/* Claim a slot for a new connection; -1 if the server is full. */
static int registry_add(server_t *srv, int fd)
{
    int slot = -1;

    pthread_mutex_lock(&srv->lock);
    if (srv->active < srv->cfg->max_clients) {
        for (slot = 0; srv->slots[slot] >= 0; slot++)
            ;
        srv->slots[slot] = fd;
        srv->active++;
    }
    pthread_mutex_unlock(&srv->lock);
    return slot;
}

static void registry_remove(server_t *srv, int slot)
{
    pthread_mutex_lock(&srv->lock);
    srv->slots[slot] = -1;
    if (--srv->active == 0)
        pthread_cond_broadcast(&srv->drained);
    pthread_mutex_unlock(&srv->lock);
}

/* ---- one connection ---------------------------------------------------- */

/*
 * Read until `buf` holds a complete request head (ending in an empty line).
 * `*used` bytes may already be buffered from a previous pipelined request.
 *
 * Returns the head length, 0 if the client closed or went idle between
 * requests (not an error), or -1 with *err_status set to a status worth
 * reporting to the client (0 if there is none).
 */
static ssize_t read_request_head(int fd, char *buf, size_t cap, size_t *used, int *err_status)
{
    size_t scanned = 0;
    *err_status = 0;

    for (;;) {
        char *end = memmem(buf + scanned, *used - scanned, "\r\n\r\n", 4);
        if (end)
            return end + 4 - buf;
        /* The terminator may straddle two reads: rescan the last 3 bytes. */
        scanned = *used >= 3 ? *used - 3 : 0;

        if (*used == cap) {
            *err_status = 431;
            return -1;
        }
        ssize_t n = recv(fd, buf + *used, cap - *used, 0);
        if (n > 0) {
            *used += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (*used == 0)
            return 0;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            *err_status = 408;          /* started a request, never finished it */
        return -1;
    }
}

/* Serve requests on one connection until either side wants to close. */
static void serve_connection(client_t *c)
{
    server_t *srv = c->srv;
    char buf[HTTP_MAX_REQUEST];
    size_t used = 0;

    for (;;) {
        int err_status;
        ssize_t head_len = read_request_head(c->fd, buf, sizeof buf, &used, &err_status);
        if (head_len <= 0) {
            if (err_status)
                log_access(c->ip, NULL, err_status,
                           http_send_error(c->fd, err_status, false, false, NULL));
            return;
        }

        http_request_t req;
        http_parse_result_t parsed = http_parse_request(buf, (size_t)head_len, &req);
        if (parsed != HTTP_PARSE_OK) {
            int status = parsed == HTTP_PARSE_BAD_VERSION ? 505 : 400;
            log_access(c->ip, NULL, status, http_send_error(c->fd, status, false, false, NULL));
            return;
        }

        /* We never read request bodies, so a request that has one is the
         * last on its connection. */
        bool keep_alive = http_wants_keep_alive(&req) && !http_has_body(&req) &&
                          !atomic_load(&srv->stopping);

        http_response_t res = handler_serve(srv->cfg->root, c->fd, &req, keep_alive);
        log_access(c->ip, &req, res.status, res.bytes);
        if (!res.keep_alive)
            return;

        /* Keep any bytes that already belong to the next pipelined request. */
        used -= (size_t)head_len;
        memmove(buf, buf + head_len, used);
    }
}

/*
 * Closing a socket that still has unread input makes the kernel send RST,
 * which can destroy the response we just wrote before the client reads it
 * (say, a 400 sent while the client is still uploading). So half-close our
 * side first and briefly swallow whatever is still arriving.
 */
static void drain_input(int fd)
{
    struct timeval timeout = { .tv_sec = LINGER_TIMEOUT_SEC };
    struct timespec start, now;
    char junk[4096];

    shutdown(fd, SHUT_WR);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        ssize_t n = recv(fd, junk, sizeof junk, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= LINGER_TIMEOUT_SEC)
            break;
    }
}

static void *client_thread(void *arg)
{
    client_t *c = arg;

    serve_connection(c);
    drain_input(c->fd);
    /* Leave the registry *before* close(), so the shutdown code never
     * touches a descriptor number that may already have been reused. */
    registry_remove(c->srv, c->slot);
    close(c->fd);
    free(c);
    return NULL;
}

/* ---- accept loop ------------------------------------------------------- */

static void accept_client(server_t *srv, int listen_fd)
{
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof addr;
    int fd = accept4(listen_fd, (struct sockaddr *)&addr, &addr_len, SOCK_CLOEXEC);

    if (fd < 0) {
        if (errno == EMFILE || errno == ENFILE) {
            /* Out of descriptors. The connection stays queued, so back off
             * instead of spinning in poll(). */
            log_error("accept: %s", strerror(errno));
            nanosleep(&(struct timespec){ .tv_nsec = 100 * 1000 * 1000 }, NULL);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR &&
                   errno != ECONNABORTED) {
            log_error("accept: %s", strerror(errno));
        }
        return;
    }
    configure_client_socket(fd);

    client_t *c = malloc(sizeof *c);
    if (!c) {
        close(fd);
        return;
    }
    c->srv = srv;
    c->fd = fd;
    inet_ntop(AF_INET, &addr.sin_addr, c->ip, sizeof c->ip);

    c->slot = registry_add(srv, fd);
    if (c->slot >= 0) {
        pthread_t thread;
        int err = pthread_create(&thread, &srv->thread_attr, client_thread, c);
        if (err == 0)
            return;
        log_error("pthread_create: %s", strerror(err));
        registry_remove(srv, c->slot);
    }

    /* Full (or out of threads): say so now rather than leave the client
     * hanging. The response is tiny, so this send never blocks. */
    log_access(c->ip, NULL, 503, http_send_error(fd, 503, false, false, "Retry-After: 1\r\n"));
    close(fd);
    free(c);
}

/* Accept connections until a signal arrives. Returns false on error. */
static bool accept_loop(server_t *srv, int listen_fd, int sig_fd)
{
    struct pollfd fds[2] = {
        { .fd = listen_fd, .events = POLLIN },
        { .fd = sig_fd,    .events = POLLIN },
    };

    for (;;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            log_error("poll: %s", strerror(errno));
            return false;
        }
        if (fds[1].revents & POLLIN) {
            struct signalfd_siginfo info;
            if (read(sig_fd, &info, sizeof info) == (ssize_t)sizeof info)
                log_info("received %s, shutting down",
                         info.ssi_signo == SIGINT ? "SIGINT" : "SIGTERM");
            return true;
        }
        if (fds[0].revents & POLLIN)
            accept_client(srv, listen_fd);
    }
}

/*
 * Let in-flight responses finish and wake up idle keep-alive connections so
 * their threads exit now rather than at the idle timeout. Returns true if
 * every connection closed within the grace period.
 */
static bool drain_connections(server_t *srv)
{
    pthread_mutex_lock(&srv->lock);
    atomic_store(&srv->stopping, true);
    if (srv->active > 0)
        log_info("waiting for %d active connection(s)", srv->active);

    /* SHUT_RD makes a blocked recv() return 0 right away. Responses that
     * are being written are unaffected, and every fd in slots[] is still
     * open because threads deregister before closing. */
    for (int i = 0; i < srv->cfg->max_clients; i++)
        if (srv->slots[i] >= 0)
            shutdown(srv->slots[i], SHUT_RD);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += SHUTDOWN_GRACE_SEC;
    while (srv->active > 0)
        if (pthread_cond_timedwait(&srv->drained, &srv->lock, &deadline) == ETIMEDOUT)
            break;

    bool drained = srv->active == 0;
    if (!drained)
        log_error("grace period over, abandoning %d connection(s)", srv->active);
    pthread_mutex_unlock(&srv->lock);
    return drained;
}

int server_run(const server_config_t *cfg)
{
    int sig_fd = setup_signals();
    if (sig_fd < 0)
        return -1;
    int listen_fd = create_listener(cfg->port);
    if (listen_fd < 0) {
        close(sig_fd);
        return -1;
    }

    /* On the heap, not the stack: if the grace period runs out, straggling
     * threads may still use it while the process exits. */
    server_t *srv = calloc(1, sizeof *srv);
    int *slots = malloc(sizeof *slots * (size_t)cfg->max_clients);
    if (!srv || !slots) {
        log_error("out of memory");
        free(srv);
        free(slots);
        close(listen_fd);
        close(sig_fd);
        return -1;
    }
    srv->cfg = cfg;
    srv->slots = slots;
    for (int i = 0; i < cfg->max_clients; i++)
        slots[i] = -1;
    atomic_init(&srv->stopping, false);
    pthread_mutex_init(&srv->lock, NULL);
    pthread_cond_init(&srv->drained, NULL);
    pthread_attr_init(&srv->thread_attr);
    pthread_attr_setdetachstate(&srv->thread_attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&srv->thread_attr, THREAD_STACK_SIZE);

    log_info("listening on http://0.0.0.0:%u, serving %s (max %d clients)",
             cfg->port, cfg->root, cfg->max_clients);
    log_info("press Ctrl+C to stop");

    bool ok = accept_loop(srv, listen_fd, sig_fd);

    close(listen_fd);           /* new connections are refused from here on */
    close(sig_fd);
    if (drain_connections(srv)) {
        pthread_attr_destroy(&srv->thread_attr);
        pthread_cond_destroy(&srv->drained);
        pthread_mutex_destroy(&srv->lock);
        free(slots);
        free(srv);
    }
    log_info("bye");
    return ok ? 0 : -1;
}

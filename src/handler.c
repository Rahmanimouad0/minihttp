/*
 * handler.c — map a request onto the document root and serve static files.
 *
 *   target ─► percent-decode ─► normalize ─► realpath() ─► inside root? ─► sendfile()
 */
#include "handler.h"
#include "mime.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>

#define INDEX_FILE "index.html"

static http_response_t send_error(int fd, int status, bool keep_alive, bool head_only,
                                  const char *extra_headers)
{
    long long bytes = http_send_error(fd, status, keep_alive, head_only, extra_headers);
    return (http_response_t){ status, bytes, keep_alive && bytes >= 0 };
}

static int status_from_errno(int err)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:      return 404;
    case EACCES:
    case EPERM:
    case ELOOP:        return 403;
    case ENAMETOOLONG: return 414;
    default:           return 500;
    }
}

/* True if `path` is `root` itself or lies somewhere below it. */
static bool is_within(const char *root, const char *path)
{
    size_t n = strlen(root);
    if (n == 1)
        return true;                                  /* root is "/" */
    return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/*
 * Open `url_path` (decoded and normalized) below `root`. Symlinks are
 * followed, but wherever they lead must still be inside the root.
 * Returns 0 and fills *out_fd and *st, or an HTTP status code on failure.
 */
static int open_in_root(const char *root, const char *url_path, int *out_fd, struct stat *st)
{
    char joined[PATH_MAX];
    char real[PATH_MAX];

    int n = snprintf(joined, sizeof joined, "%s%s", root, url_path);
    if (n < 0 || (size_t)n >= sizeof joined)
        return 414;
    if (!realpath(joined, real))
        return status_from_errno(errno);
    if (!is_within(root, real))
        return 403;

    /* O_NONBLOCK so a FIFO in the root can't hang the thread in open();
     * it has no effect on regular files. */
    int fd = open(real, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return status_from_errno(errno);
    if (fstat(fd, st) < 0) {
        close(fd);
        return 500;
    }
    *out_fd = fd;
    return 0;
}

/* "/docs" names a directory: send the client to "/docs/" so that relative
 * links inside its index page resolve against the right base. */
static http_response_t redirect_to_directory(int fd, const http_request_t *req,
                                             bool keep_alive, bool head_only)
{
    /* The raw target is validated printable ASCII, so echoing it in a
     * header can't inject anything. */
    const char *target = req->target;
    size_t path_len = strcspn(target, "?#");
    const char *query = target[path_len] == '?' ? target + path_len : "";
    char location[HTTP_MAX_REQUEST + 32];

    snprintf(location, sizeof location, "Location: %.*s/%s\r\n", (int)path_len, target, query);
    return send_error(fd, 301, keep_alive, head_only, location);
}

static http_response_t send_file(int fd, int file, const struct stat *st, const char *type,
                                 bool keep_alive, bool head_only)
{
    http_response_t res = { 200, 0, keep_alive };
    char modified[64];
    char extra[96];

    http_format_date(st->st_mtime, modified, sizeof modified);
    snprintf(extra, sizeof extra, "Last-Modified: %s\r\n", modified);

    bool has_body = !head_only && st->st_size > 0;
    if (http_send_head(fd, 200, type, (long long)st->st_size, keep_alive, extra, has_body) < 0) {
        res.bytes = -1;
        res.keep_alive = false;
        return res;
    }
    if (!has_body)
        return res;

    /* sendfile() moves the data from the page cache straight to the socket,
     * with no copy through a user-space buffer. */
    off_t offset = 0;
    while (offset < st->st_size) {
        ssize_t n = sendfile(fd, file, &offset, (size_t)(st->st_size - offset));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;          /* client gone, send timeout, or the file shrank */
    }
    res.bytes = offset;
    if (offset < st->st_size)
        res.keep_alive = false;   /* we promised more bytes than we sent */
    return res;
}

http_response_t handler_serve(const char *root, int fd, const http_request_t *req, bool keep_alive)
{
    bool head_only = req->method == HTTP_HEAD;

    if (req->method == HTTP_OTHER)
        return send_error(fd, 405, keep_alive, false, "Allow: GET, HEAD\r\n");

    char decoded[PATH_MAX];
    char path[PATH_MAX];
    int rc = http_decode_path(req->target, decoded, sizeof decoded);
    if (rc == 0)
        rc = http_normalize_path(decoded, path, sizeof path);
    if (rc != 0)   /* -1: malformed or escapes the root, -2: too long */
        return send_error(fd, rc == -2 ? 414 : 400, keep_alive, head_only, NULL);

    int file;
    struct stat st;
    int status = open_in_root(root, path, &file, &st);
    if (status != 0)
        return send_error(fd, status, keep_alive, head_only, NULL);

    if (S_ISDIR(st.st_mode)) {
        close(file);
        size_t len = strlen(path);
        if (path[len - 1] != '/')
            return redirect_to_directory(fd, req, keep_alive, head_only);
        if (len + sizeof INDEX_FILE > sizeof path)
            return send_error(fd, 414, keep_alive, head_only, NULL);
        memcpy(path + len, INDEX_FILE, sizeof INDEX_FILE);

        /* No index.html means 404: directory listings are not served. */
        status = open_in_root(root, path, &file, &st);
        if (status != 0)
            return send_error(fd, status, keep_alive, head_only, NULL);
    }

    if (!S_ISREG(st.st_mode)) {       /* device, FIFO, socket, ... */
        close(file);
        return send_error(fd, 403, keep_alive, head_only, NULL);
    }

    http_response_t res = send_file(fd, file, &st, mime_type(path), keep_alive, head_only);
    close(file);
    return res;
}

/*
 * minihttp — a tiny HTTP/1.1 static file server.
 *
 *   ./minihttp --port 8080 --root ./public
 */
#include "server.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEFAULT_PORT         8080
#define DEFAULT_ROOT         "./public"
#define DEFAULT_MAX_CLIENTS  256

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [--port PORT] [--root DIR] [--max-clients N]\n"
            "\n"
            "Serve static files from DIR over HTTP/1.1.\n"
            "\n"
            "  -p, --port PORT        TCP port to listen on (default %d)\n"
            "  -r, --root DIR         document root (default %s)\n"
            "  -c, --max-clients N    concurrent connection limit (default %d)\n"
            "  -h, --help             show this help\n",
            prog, DEFAULT_PORT, DEFAULT_ROOT, DEFAULT_MAX_CLIENTS);
}

/* Parse a base-10 integer in [min, max]; false on junk or overflow. */
static bool parse_int(const char *s, long min, long max, long *out)
{
    char *end;
    errno = 0;
    long value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || value < min || value > max)
        return false;
    *out = value;
    return true;
}

int main(int argc, char **argv)
{
    /* Static: worker threads keep using these until the process exits. */
    static char root[PATH_MAX];
    static server_config_t cfg;

    const char *root_arg = DEFAULT_ROOT;
    long port = DEFAULT_PORT;
    long max_clients = DEFAULT_MAX_CLIENTS;

    static const struct option long_opts[] = {
        { "port",        required_argument, NULL, 'p' },
        { "root",        required_argument, NULL, 'r' },
        { "max-clients", required_argument, NULL, 'c' },
        { "help",        no_argument,       NULL, 'h' },
        { NULL,          0,                 NULL, 0   },
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:r:c:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'p':
            if (!parse_int(optarg, 1, 65535, &port)) {
                fprintf(stderr, "%s: invalid port '%s'\n", argv[0], optarg);
                return 2;
            }
            break;
        case 'r':
            root_arg = optarg;
            break;
        case 'c':
            if (!parse_int(optarg, 1, 100000, &max_clients)) {
                fprintf(stderr, "%s: invalid --max-clients '%s'\n", argv[0], optarg);
                return 2;
            }
            break;
        case 'h':
            usage(stdout, argv[0]);
            return 0;
        default:
            usage(stderr, argv[0]);
            return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "%s: unexpected argument '%s'\n", argv[0], argv[optind]);
        usage(stderr, argv[0]);
        return 2;
    }

    /* Canonicalize once so every request can be checked against it. */
    struct stat st;
    if (!realpath(root_arg, root) || stat(root, &st) < 0) {
        fprintf(stderr, "%s: cannot use root '%s': %s\n", argv[0], root_arg, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s: root '%s' is not a directory\n", argv[0], root_arg);
        return 1;
    }

    /* Line-buffered so access logs show up promptly when redirected. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    cfg = (server_config_t){
        .port = (uint16_t)port,
        .root = root,
        .max_clients = (int)max_clients,
    };
    return server_run(&cfg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

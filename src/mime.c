/*
 * mime.c — file extension → Content-Type.
 */
#include "mime.h"

#include <string.h>
#include <strings.h>

static const struct {
    const char *ext;
    const char *type;
} types[] = {
    { "html",  "text/html; charset=utf-8" },
    { "htm",   "text/html; charset=utf-8" },
    { "css",   "text/css; charset=utf-8" },
    { "js",    "text/javascript; charset=utf-8" },
    { "mjs",   "text/javascript; charset=utf-8" },
    { "json",  "application/json" },
    { "txt",   "text/plain; charset=utf-8" },
    { "md",    "text/markdown; charset=utf-8" },
    { "xml",   "application/xml" },
    { "svg",   "image/svg+xml" },
    { "png",   "image/png" },
    { "jpg",   "image/jpeg" },
    { "jpeg",  "image/jpeg" },
    { "gif",   "image/gif" },
    { "webp",  "image/webp" },
    { "ico",   "image/x-icon" },
    { "pdf",   "application/pdf" },
    { "wasm",  "application/wasm" },
    { "woff",  "font/woff" },
    { "woff2", "font/woff2" },
    { "mp4",   "video/mp4" },
    { "zip",   "application/zip" },
};

const char *mime_type(const char *path)
{
    const char *base = strrchr(path, '/');
    const char *dot = strrchr(base ? base : path, '.');

    if (dot) {
        for (size_t i = 0; i < sizeof types / sizeof *types; i++)
            if (strcasecmp(dot + 1, types[i].ext) == 0)
                return types[i].type;
    }
    return "application/octet-stream";
}

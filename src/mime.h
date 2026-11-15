#ifndef MINIHTTP_MIME_H
#define MINIHTTP_MIME_H

/* Content-Type for a file, chosen by extension (case-insensitive).
 * Unknown extensions get "application/octet-stream". */
const char *mime_type(const char *path);

#endif

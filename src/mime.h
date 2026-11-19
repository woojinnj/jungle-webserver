#ifndef MIME_H
#define MIME_H

/* Content-Type for a file path, by extension (case-insensitive).
 * Unknown/missing extension -> application/octet-stream. */
const char *mime_type(const char *path);

#endif

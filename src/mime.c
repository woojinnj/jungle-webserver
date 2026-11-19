#include "mime.h"

#include <string.h>
#include <strings.h>

/* 확장자 → MIME 타입 표 */
static const struct {
    const char *ext;
    const char *type;
} MIME_TABLE[] = {
    { ".html", "text/html; charset=utf-8" },
    { ".htm",  "text/html; charset=utf-8" },
    { ".css",  "text/css; charset=utf-8" },
    { ".js",   "application/javascript; charset=utf-8" },
    { ".json", "application/json" },
    { ".txt",  "text/plain; charset=utf-8" },
    { ".png",  "image/png" },
    { ".jpg",  "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".gif",  "image/gif" },
    { ".svg",  "image/svg+xml" },
    { ".ico",  "image/x-icon" },
};

#define DEFAULT_MIME "application/octet-stream"

/* 파일 경로의 확장자로 Content-Type 을 고른다. 모르면 octet-stream.
 * CSAPP 11.5.2 (MIME types) / 11.6 (get_filetype). */
const char *mime_type_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    const char *slash = strrchr(path, '/');

    if (dot == NULL || (slash != NULL && dot < slash))
        return DEFAULT_MIME;
    for (size_t i = 0; i < sizeof(MIME_TABLE) / sizeof(MIME_TABLE[0]); i++) {
        if (strcasecmp(dot, MIME_TABLE[i].ext) == 0)
            return MIME_TABLE[i].type;
    }
    return DEFAULT_MIME;
}

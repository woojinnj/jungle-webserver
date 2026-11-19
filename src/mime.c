#include "mime.h"

#include <string.h>

#include "http.h"

static const struct {
    const char *ext;
    const char *type;
} table[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "text/javascript; charset=utf-8"},
    {"mjs", "text/javascript; charset=utf-8"},
    {"json", "application/json"},
    {"txt", "text/plain; charset=utf-8"},
    {"xml", "application/xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},
    {"webp", "image/webp"},
    {"pdf", "application/pdf"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"mp3", "audio/mpeg"},
    {"mp4", "video/mp4"},
    {"wasm", "application/wasm"},
};

const char *mime_type(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(path, '.');
    if (dot && (!slash || dot > slash)) {
        dot++;
        for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
            if (http_ieq(dot, strlen(dot), table[i].ext)) return table[i].type;
    }
    return "application/octet-stream";
}

#ifndef STATIC_H
#define STATIC_H

#include "net.h"

const char *static_mime_type(const char *path);
void static_serve(int client, int root, const char *path, Deadline deadline);

#endif

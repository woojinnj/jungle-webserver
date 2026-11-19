#ifndef PROXY_H
#define PROXY_H

#include <stddef.h>

#include "http.h"
#include "router.h"

/* Reverse-proxies one request to a backend of rt (round-robin; if connecting
 * fails, the next backend is tried). Any method is forwarded.
 * Errors before the response starts: 502 (refused / bad reply), 504 (connect
 * or read timeout). *keep is in/out: cleared when the client connection must
 * close afterwards (close-delimited / chunked upstream reply, truncated body).
 * Returns the status sent to the client; *bytes = body bytes relayed. */
int proxy_handle(int cfd, route *rt, const http_request *req, const char *body, size_t blen,
                 const char *client_ip, int read_timeout_ms, int *keep, size_t *bytes);

#endif

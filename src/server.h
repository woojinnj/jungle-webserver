#ifndef SERVER_H
#define SERVER_H

#include "config.h"

/* Listens on cfg->port, serves with a thread pool until server_request_stop()
 * (async-signal-safe). Returns 0 after a clean shutdown, 1 on setup failure. */
int server_run(const config *cfg);
void server_request_stop(void);

#endif

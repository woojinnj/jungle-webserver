#ifndef NET_H
#define NET_H

int net_open_listenfd(int port);
int net_connect_timeout(const char *host, const char *port, int timeout_sec);
int net_set_timeout(int fd, int timeout_sec);
int net_set_cloexec(int fd);

#endif

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include "config.h"

typedef void (*conn_handler)(int fd, void *arg);

/* 고정 개수 작업 스레드 + 연결 fd 를 담는 원형 큐 (생산자-소비자) */
typedef struct threadpool {
    pthread_t *threads;
    int thread_count;
    int queue[QUEUE_CAPACITY];
    int head, tail, size;
    int shutting_down;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    conn_handler handler;
    void *handler_arg;
} threadpool;

int threadpool_init(threadpool *pool, int thread_count, conn_handler handler, void *arg);
int threadpool_submit(threadpool *pool, int fd);
int threadpool_has_waiting(threadpool *pool);
void threadpool_destroy(threadpool *pool);

#endif

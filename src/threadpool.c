#include "threadpool.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

/* 큐에서 fd 하나를 꺼낸다. 큐가 비어 있으면 기다린다. 종료 중이고 비었으면 -1.
 * CSAPP 12.5.4 (sbuf_remove): 생산자-소비자 문제. */
static int dequeue_fd(threadpool *pool)
{
    int fd;

    pthread_mutex_lock(&pool->lock);
    while (pool->size == 0 && !pool->shutting_down)
        pthread_cond_wait(&pool->not_empty, &pool->lock);
    if (pool->size == 0) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }
    fd = pool->queue[pool->head];
    pool->head = (pool->head + 1) % QUEUE_CAPACITY;
    pool->size--;
    pthread_cond_signal(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);
    return fd;
}

/* 작업 스레드: 큐에서 연결을 꺼내 처리하기를 반복한다.
 * CSAPP 12.5.5 (prethreaded concurrent server). */
static void *worker_main(void *arg)
{
    threadpool *pool = arg;
    int fd;

    while ((fd = dequeue_fd(pool)) >= 0)
        pool->handler(fd, pool->handler_arg);
    return NULL;
}

/* 작업 스레드는 SIGINT/SIGTERM 을 막아 두어, 시그널은 메인 스레드(accept)가 받게 한다.
 * CSAPP 12.7 / 8.5: 멀티스레드에서 시그널은 막지 않은 아무 스레드에 전달된다. */
static int start_workers_with_signals_blocked(threadpool *pool)
{
    sigset_t block, old;
    int started = 0;

    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &block, &old) != 0)
        return 0;
    while (started < pool->thread_count &&
           pthread_create(&pool->threads[started], NULL, worker_main, pool) == 0)
        started++;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return started;
}

/* 스레드 풀을 만들고 작업 스레드를 시작한다. 0 성공, -1 실패.
 * 스레드를 미리 만들어 두면 요청마다 생성 비용이 들지 않는다. */
int threadpool_init(threadpool *pool, int thread_count, conn_handler handler, void *arg)
{
    pool->threads = malloc(sizeof(pthread_t) * (size_t)thread_count);
    if (pool->threads == NULL)
        return -1;
    pool->thread_count = thread_count;
    pool->head = pool->tail = pool->size = 0;
    pool->shutting_down = 0;
    pool->handler = handler;
    pool->handler_arg = arg;
    if (pthread_mutex_init(&pool->lock, NULL) != 0 ||
        pthread_cond_init(&pool->not_empty, NULL) != 0 ||
        pthread_cond_init(&pool->not_full, NULL) != 0) {
        free(pool->threads);
        return -1;
    }
    pool->thread_count = start_workers_with_signals_blocked(pool);
    if (pool->thread_count == 0) {
        threadpool_destroy(pool);
        return -1;
    }
    return 0;
}

/* 연결 fd 를 큐에 넣는다. 큐가 꽉 차면 빈자리가 날 때까지 기다린다. 종료 중이면 -1.
 * CSAPP 12.5.4 (sbuf_insert). */
int threadpool_submit(threadpool *pool, int fd)
{
    pthread_mutex_lock(&pool->lock);
    while (pool->size == QUEUE_CAPACITY && !pool->shutting_down)
        pthread_cond_wait(&pool->not_full, &pool->lock);
    if (pool->shutting_down) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }
    pool->queue[pool->tail] = fd;
    pool->tail = (pool->tail + 1) % QUEUE_CAPACITY;
    pool->size++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

/* 큐에서 처리를 기다리는 연결이 있는지 확인한다.
 * keep-alive 연결이 스레드를 독점해 대기 연결이 굶는 것(starvation)을 막는 데 쓴다. */
int threadpool_has_waiting(threadpool *pool)
{
    int waiting;

    pthread_mutex_lock(&pool->lock);
    waiting = pool->size > 0;
    pthread_mutex_unlock(&pool->lock);
    return waiting;
}

/* 종료 표시 → 모든 스레드 깨우기 → 남은 작업을 끝낸 스레드들을 join → 자원 해제.
 * 안전한 셧다운: 처리 중인 요청은 끝까지 마친다. */
void threadpool_destroy(threadpool *pool)
{
    pthread_mutex_lock(&pool->lock);
    pool->shutting_down = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < pool->thread_count; i++)
        pthread_join(pool->threads[i], NULL);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    free(pool->threads);
    pool->threads = NULL;
}

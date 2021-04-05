#include "tpool.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include "list.h"

enum __future_flags {
    __FUTURE_RUNNING = 01,
    __FUTURE_FINISHED = 02,
    __FUTURE_TIMEOUT = 04,
    __FUTURE_CANCELLED = 010,
    __FUTURE_DESTROYED = 020,
};

/* Linked-list to store the jobs in queue */
typedef struct __threadtask {
    void *(*func)(void *); /* the function that task should execute */
    void *arg;             /* argument passed to func */
    struct __tpool_future
        *future;           /* A structure to store task status and result */
    struct list_head list; /* linked-list of task structure */
} threadtask_t;

typedef struct __jobqueue {
    struct list_head head; /* list head of task */
    pthread_cond_t
        cond_nonempty; /* condition variable to check if queue is non-empty */
    pthread_mutex_t rwlock; /* lock share resources like head and tail */
} jobqueue_t;

/* A structure to store job status and return result */
struct __tpool_future {
    int flag;              /* job status */
    void *result;          /* the result of job which points to this future */
    pthread_mutex_t mutex; /* lock share resouces like flag and result */
    pthread_cond_t
        cond_finished; /* condition variable to check if job is finished */
};

struct __threadpool {
    size_t count;         /* count of pthread */
    pthread_t *workers;   /* store pthread id */
    jobqueue_t *jobqueue; /* queue which stores pending jobs */
};

static struct __tpool_future *tpool_future_create(void)
{
    struct __tpool_future *future = malloc(sizeof(struct __tpool_future));
    if (future) {
        future->flag = 0;
        future->result = NULL;
        pthread_mutex_init(&future->mutex, NULL);
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_cond_init(&future->cond_finished, &attr);
        pthread_condattr_destroy(&attr);
    }
    return future;
}

int tpool_future_destroy(struct __tpool_future *future)
{
    if (future) {
        pthread_mutex_lock(&future->mutex);
        if (future->flag & __FUTURE_FINISHED ||
            future->flag & __FUTURE_CANCELLED) {
            pthread_mutex_unlock(&future->mutex);
            pthread_mutex_destroy(&future->mutex);
            pthread_cond_destroy(&future->cond_finished);
            free(future);
        } else {
            future->flag |= __FUTURE_DESTROYED;
            pthread_mutex_unlock(&future->mutex);
        }
    }
    return 0;
}

void *tpool_future_get(struct __tpool_future *future, unsigned int seconds)
{
    pthread_mutex_lock(&future->mutex);
    /* turn off the timeout bit set previously */
    future->flag &= ~__FUTURE_TIMEOUT;
    while ((future->flag & __FUTURE_FINISHED) == 0) {
        if (seconds) {
            struct timespec expire_time;
            clock_gettime(CLOCK_MONOTONIC, &expire_time);
            expire_time.tv_sec += seconds;
            int status = pthread_cond_timedwait(&future->cond_finished,
                                                &future->mutex, &expire_time);
            if (status == ETIMEDOUT) {
                future->flag |= __FUTURE_TIMEOUT;
                pthread_mutex_unlock(&future->mutex);
                return NULL;
            }
        } else
            pthread_cond_wait(&future->cond_finished, &future->mutex);
    }

    pthread_mutex_unlock(&future->mutex);
    return future->result;
}

static jobqueue_t *jobqueue_create(void)
{
    jobqueue_t *jobqueue = malloc(sizeof(jobqueue_t));
    if (jobqueue) {
        INIT_LIST_HEAD(&jobqueue->head);
        pthread_cond_init(&jobqueue->cond_nonempty, NULL);
        pthread_mutex_init(&jobqueue->rwlock, NULL);
    }
    return jobqueue;
}

static void jobqueue_destroy(jobqueue_t *jobqueue)
{
    struct list_head *node, *tmp, *head;
    head = &jobqueue->head;
    list_for_each_safe(node, tmp, head)
    {
        threadtask_t *task = list_entry(node, threadtask_t, list);
        pthread_mutex_lock(&task->future->mutex);
        if (task->future->flag & __FUTURE_DESTROYED) {
            pthread_mutex_unlock(&task->future->mutex);
            pthread_mutex_destroy(&task->future->mutex);
            pthread_cond_destroy(&task->future->cond_finished);
            free(task->future);
        } else {
            task->future->flag |= __FUTURE_CANCELLED;
            pthread_mutex_unlock(&task->future->mutex);
        }
        free(task);
    }

    pthread_mutex_destroy(&jobqueue->rwlock);
    pthread_cond_destroy(&jobqueue->cond_nonempty);
    free(jobqueue);
}

static void __jobqueue_fetch_cleanup(void *arg)
{
    pthread_mutex_t *mutex = (pthread_mutex_t *) arg;
    pthread_mutex_unlock(mutex);
}

static void *jobqueue_fetch(void *queue)
{
    jobqueue_t *jobqueue = (jobqueue_t *) queue;
    struct list_head *head = &jobqueue->head;
    threadtask_t *task;
    int old_state;

    pthread_cleanup_push(__jobqueue_fetch_cleanup, (void *) &jobqueue->rwlock);

    while (1) {
        pthread_mutex_lock(&jobqueue->rwlock);
        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, &old_state);
        pthread_testcancel();

        while (list_empty(head))
            pthread_cond_wait(&jobqueue->cond_nonempty, &jobqueue->rwlock);
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
        task = list_entry(head->prev, threadtask_t, list);
        list_del(head->prev);
        pthread_mutex_unlock(&jobqueue->rwlock);

        if (task->func) {
            pthread_mutex_lock(&task->future->mutex);
            if (task->future->flag & __FUTURE_CANCELLED) {
                pthread_mutex_unlock(&task->future->mutex);
                free(task);
                continue;
            } else {
                task->future->flag |= __FUTURE_RUNNING;
                pthread_mutex_unlock(&task->future->mutex);
            }

            void *ret_value = task->func(task->arg);
            pthread_mutex_lock(&task->future->mutex);
            if (task->future->flag & __FUTURE_DESTROYED) {
                pthread_mutex_unlock(&task->future->mutex);
                pthread_mutex_destroy(&task->future->mutex);
                pthread_cond_destroy(&task->future->cond_finished);
                free(task->future);
            } else {
                task->future->flag |= __FUTURE_FINISHED;
                task->future->result = ret_value;
                pthread_cond_broadcast(&task->future->cond_finished);
                pthread_mutex_unlock(&task->future->mutex);
            }
            free(task);
        } else {
            pthread_mutex_destroy(&task->future->mutex);
            pthread_cond_destroy(&task->future->cond_finished);
            free(task->future);
            free(task);
            break;
        }
    }

    pthread_cleanup_pop(0);
    pthread_exit(NULL);
}

struct __threadpool *tpool_create(size_t count)
{
    jobqueue_t *jobqueue = jobqueue_create();
    struct __threadpool *pool = malloc(sizeof(struct __threadpool));
    if (!jobqueue || !pool) {
        if (jobqueue)
            jobqueue_destroy(jobqueue);
        free(pool);
        return NULL;
    }

    pool->count = count, pool->jobqueue = jobqueue;
    if ((pool->workers = malloc(count * sizeof(pthread_t)))) {
        for (int i = 0; i < count; i++) {
            if (pthread_create(&pool->workers[i], NULL, jobqueue_fetch,
                               (void *) jobqueue)) {
                for (int j = 0; j < i; j++)
                    pthread_cancel(pool->workers[j]);
                for (int j = 0; j < i; j++)
                    pthread_join(pool->workers[j], NULL);
                free(pool->workers);
                jobqueue_destroy(jobqueue);
                free(pool);
                return NULL;
            }
        }
        return pool;
    }

    jobqueue_destroy(jobqueue);
    free(pool);
    return NULL;
}

struct __tpool_future *tpool_apply(struct __threadpool *pool,
                                   void *(*func)(void *),
                                   void *arg)
{
    jobqueue_t *jobqueue = pool->jobqueue;
    threadtask_t *new_head = malloc(sizeof(threadtask_t));
    struct __tpool_future *future = tpool_future_create();
    if (new_head && future) {
        new_head->func = func, new_head->arg = arg, new_head->future = future;
        pthread_mutex_lock(&jobqueue->rwlock);
        list_add(&new_head->list, &jobqueue->head);
        if (list_is_singular(&jobqueue->head)) {
            pthread_cond_broadcast(&jobqueue->cond_nonempty);
        }
        pthread_mutex_unlock(&jobqueue->rwlock);
    } else if (new_head) {
        free(new_head);
        return NULL;
    } else if (future) {
        tpool_future_destroy(future);
        return NULL;
    }
    return future;
}

int tpool_join(struct __threadpool *pool)
{
    size_t num_threads = pool->count;
    for (int i = 0; i < num_threads; i++)
        tpool_apply(pool, NULL, NULL);
    for (int i = 0; i < num_threads; i++)
        pthread_join(pool->workers[i], NULL);
    free(pool->workers);
    jobqueue_destroy(pool->jobqueue);
    free(pool);
    return 0;
}

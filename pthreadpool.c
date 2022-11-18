/*
 * Author: chenhui
 * email: 755672829@qq.com
 * github: https://github.com/chen755672829
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <signal.h>
#include <errno.h>

#include <time.h>
#include <unistd.h>
#include <pthread.h>

typedef pthread_mutex_t  thread_mutex_t;

typedef pthread_cond_t  thread_cond_t;

/*typedef 语句定义的类型一般用_t作后缀， 而enum类型一般以_e作后缀！*/
typedef void (*task_callback_t)(void *);

typedef struct task {
    struct task *next;
    task_callback_t func;
    void *arg;
} task_t;

typedef struct worker {
    struct worker *active_next;
    pthread_t active_tid;
} worker_t;

typedef struct pthreadpool {
    struct pthreadpool *forw;
    struct pthreadpool *back;

    pthread_mutex_t mtx;

    // 这个条件变量，在线程要销毁时，flags打上标记销毁标记POOL_DESTROY，然后发送销毁所有线程的命令，
    // 然后条件变量busycv进行等待时，当线程退出时，执行WorkerCleanup函数时，这个函数里面会判断当前的线程是否都已经销毁，
    // 若已经全部销毁，就进行唤醒条件变量busycv。
    pthread_cond_t busycv;  

    /* 当创建的线程，处于空闲状态时，这个条件变量workcv进行等待*/
    pthread_cond_t workcv;

    /*
        当调用pThreadPool_tWait函数时，等待所有任务执行完成。
        调用pThreadPool_tWait函数，若当前任务队列中，有任务或任务正在执行中，flags就会打上POOL_WAIT标记，
        表示，当前有线程正在等待所有任务执行完成。
    */
    pthread_cond_t waitcv;

    worker_t *active;         // 当前已创建的线程
    task_t *head;             // 工作队列的头部
    task_t *tail;             // 工作队列的尾部

    pthread_attr_t attr;    // 线程属性

    int flags;              // 标记位
    unsigned int linger;    // 当线程数大于最小线程数时，且队列中，没有需要task时，当前线程需要等待的时间，若超时，就会把当前线程销毁

    int minimum;    // 设置的最小线程数
    int maximum;    // s设置的最大线程数
    int nthreads;   // 当前已经创建且存在的线程数
    int idle;   //若idle大于0，就说明有空闲的线程。
} pThreadPool_t;

static void* WorkerThread(void *arg);

#define POOL_WAIT			0x01
#define POOL_DESTROY		0x02

static pthread_mutex_t thread_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static sigset_t fillset;
pThreadPool_t *thread_pool = NULL;

static int WorkerCreate(pThreadPool_t *pool) {

    sigset_t oset;
    pthread_t thread_id;
    if(pool->flags & POOL_DESTROY) return -1;
    /*先屏蔽所有信号*/
    pthread_sigmask(SIG_SETMASK, &fillset, &oset);
    int error = pthread_create(&thread_id, &pool->attr, WorkerThread, pool);
    /*然后创建完线程后，再好本来的线程信号，设置上*/
    pthread_sigmask(SIG_SETMASK, &oset, NULL);
    return error;
}


static void WorkerCleanup(pThreadPool_t * pool) {
    printf("%s pool->nthreads==%d\n", __func__, pool->nthreads);
    --pool->nthreads;

    /*当前线程要结束了，所以在维护的工作线程链表中，也要删除掉*/
    pthread_t tid = pthread_self();
    worker_t *active;
    worker_t **activepp;
    // 删除链表中的元素，使用双指针，记录指向 当前active结构 的指针地址。
    for (active = pool->active, activepp = &pool->active;
        active != NULL ;
        activepp = &active->active_next, active = active->active_next) {
        if (active->active_tid == tid) {
            *activepp = active->active_next;
            /*  根本不用维护链表头，上面一行已经维护了 */
#if 0
            if (pool->active == active) {
                pool->active = active->active_next;
            }
#endif
            active->active_next = NULL;
            free(active);
            break;
        }
    }

    // 若用户调用 pThreadPool_tDestroy 要销毁线程。
    if (pool->flags & POOL_DESTROY) {
        if (pool->nthreads == 0) {
            pthread_cond_broadcast(&pool->busycv);
        }
    } else if (pool->head != NULL && pool->nthreads < pool->maximum && WorkerCreate(pool) == 0) {
        printf("*********WorkerCreate %s pool->nthreads==%d\n", __func__, pool->nthreads);
        pool->nthreads ++;
    }
    pthread_mutex_unlock(&pool->mtx);
}


static inline void NotifyWaiters(pThreadPool_t *pool) {
    if (pool->head == NULL && pool->idle == pool->nthreads) {
        pool->flags &= ~POOL_WAIT;
        pthread_cond_broadcast(&pool->waitcv);
    }
}


static void JobCleanup(pThreadPool_t *pool) {
    pthread_mutex_lock(&pool->mtx);
    if (pool->flags & POOL_WAIT) NotifyWaiters(pool);
}


static void* WorkerThread(void *arg) {
    pThreadPool_t *pool = (pThreadPool_t*)arg;
    worker_t *active = malloc(sizeof(worker_t));

    int timeout;
    struct timespec ts;
    task_callback_t func;

    pthread_sigmask(SIG_SETMASK, &fillset, NULL);
    // 线程取消相关
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    pthread_cleanup_push(WorkerCleanup, (void *)pool);
    pthread_mutex_lock(&pool->mtx);

    // 维护创建线程id的链表
    active->active_tid = pthread_self();
    active->active_next = pool->active;
    pool->active = active;

    while (1) {
        timeout = 0;
        pool->idle ++;

        if (pool->flags & POOL_WAIT) {
            NotifyWaiters(pool);
        }

        while (pool->head == NULL && !(pool->flags & POOL_DESTROY)) {
            printf("---%s---pool->head == NULL---\n", __func__);
            if (pool->nthreads <= pool->minimum) {
                pthread_cond_wait(&pool->workcv, &pool->mtx);
            } else {
                // 当线程池的线程数大于最小线程数时，按照设置的时间进行线程等待时间
                clock_gettime(CLOCK_MONOTONIC, &ts);
                ts.tv_sec += pool->linger;
                if (pool->linger == 0 || pthread_cond_timedwait(&pool->workcv, &pool->mtx, &ts) == ETIMEDOUT) {
                    timeout = 1;
                    break;
                }
            }
        }
        pool->idle --;
        if (pool->flags & POOL_DESTROY) break;

        task_t *task = pool->head;
        if (task != NULL) {
            timeout = 0;
            func = task->func;

            void *task_arg = task->arg;
            pool->head = task->next;

            if (task == pool->tail) {
            	pool->tail = NULL;
            }
            pthread_mutex_unlock(&pool->mtx);

            pthread_cleanup_push(JobCleanup, pool);
            free(task);
            func(task_arg);
            pthread_cleanup_pop(1);
        }
        // 这一步很重要啊，若超时且当前线程数大于 最小的线程数，这样才销毁当前线程
        if (timeout && (pool->nthreads > pool->minimum)) {
            break;
        }
    }
    pthread_cleanup_pop(1);
    return NULL;
}

/*
    克隆线程属性， 设置初始化并设置线程属性
*/
static void CloneAttributes(pthread_attr_t *new_attr, pthread_attr_t *old_attr) {

    struct sched_param param;
    void *addr;
    size_t size;
    int value;

    pthread_attr_init(new_attr);

    if (old_attr != NULL) {
        /*设置线程栈的地址和大小*/
        pthread_attr_getstack(old_attr, &addr, &size);
        pthread_attr_setstack(new_attr, NULL, size);

        /*scope属性表示线程间竞争CPU的范围*/
        /*PTHREAD_SCOPE_SYSTEM和PTHREAD_SCOPE_PROCESS，前者表示与系统中所有线程一起竞争CPU时间，
        后者表示仅与同进程中的线程竞争CPU。默认为PTHREAD_SCOPE_PROCESS
        目前LinuxThreads仅实现了PTHREAD_SCOPE_SYSTEM一值
        */
        pthread_attr_getscope(old_attr, &value);
        pthread_attr_setscope(new_attr, value);

        pthread_attr_getinheritsched(old_attr, &value);
        pthread_attr_setinheritsched(new_attr, value);

        pthread_attr_getschedpolicy(old_attr, &value);
        pthread_attr_setschedpolicy(new_attr, value);

        pthread_attr_getschedparam(old_attr, &param);
        pthread_attr_setschedparam(new_attr, &param);

        pthread_attr_getguardsize(old_attr, &size);
        pthread_attr_setguardsize(new_attr, size);
    }
    // 设置为与主线程分离的状态。
    pthread_attr_setdetachstate(new_attr, PTHREAD_CREATE_DETACHED);
}


int thread_mutex_create(thread_mutex_t *mtx)
{
    int err;
    pthread_mutexattr_t  attr;
    err = pthread_mutexattr_init(&attr);
    if (err != 0) {
        return -1;
    }
    /***
     * 2.检错锁（PTHREAD_MUTEX_ERRORCHECK）
     * 一个线程如果对一个已经加锁的检错锁再次加锁，则加锁操作返回EDEADLK；
     * 对一个已经被其他线程加锁的检错锁解锁或者对一个已经解锁的检错锁再次解锁，则解锁操作返回EPERM；
    ***/
    err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    if (err != 0) {
        return -1;
    }

    err = pthread_mutex_init(mtx, &attr);
    if (err != 0) {
       return -1;
    }
    err = pthread_mutexattr_destroy(&attr);
    if (err != 0) {
        return -1;
    }
    return 0;
}


int thread_cond_create(thread_cond_t *cond)
{
    // 函数返回值，错误处理注意完善。
    int err;
    pthread_condattr_t attr;
    err = pthread_condattr_init(&attr);
    if (err != 0) {
        return -1;
    }

#if 0 
    clockid_t clock_id;
    pthread_condattr_getclock(&attr, &clock_id);
    printf("clock_id: %d\n", clock_id);
#endif
    err = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (err != 0) {
        return -1;
    }

    err = pthread_cond_init(cond, &attr);
    if (err != 0) {
        return -1;
    }

    err = pthread_condattr_destroy(&attr);
    if (err != 0) {
        return -1;
    }
    return 0;
}


/***
 * brief: 创建一个线程池
 * @param1 [in] int min_threads 最小线程数
 * @param2 [in] int max_threads 最大线程数
 * @param3 [in] int linger  
 * @param4 [in] pthread_attr_t *attr  线程属性
 * return 一个线程池 pThreadPool_t *
***/
pThreadPool_t *ThreadPoolCreate(int min_threads, int max_threads, int linger, pthread_attr_t *attr) {

    /*sigfillset用来将参数set信号集初始化，然后把所有的信号加入到此信号集里即将所有的信号标志位置为1，屏蔽所有的信号
        #define sigfillset(ptr) ( *(ptr) = ~(sigset_t)0, 0)
    */
    sigfillset(&fillset);
    if (min_threads > max_threads || max_threads < 1) {
        errno = EINVAL;
        return NULL;
    }

    pThreadPool_t *pool = (pThreadPool_t*)malloc(sizeof(pThreadPool_t));
    if (pool == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    thread_mutex_create(&pool->mtx);
    //pthread_mutex_init(&pool->mtx, NULL);

    thread_cond_create(&pool->workcv);
    //pthread_cond_init(&pool->workcv, NULL);
    pthread_cond_init(&pool->busycv, NULL);
    pthread_cond_init(&pool->waitcv, NULL);

    pool->active = NULL;
    pool->head = NULL;
    pool->tail = NULL;
    pool->flags = 0;
    pool->linger = linger;
    pool->minimum = min_threads;
    pool->maximum = max_threads;
    pool->nthreads = 0;
    pool->idle = 0;

    CloneAttributes(&pool->attr, attr);

    pthread_mutex_lock(&thread_pool_lock);
    if (thread_pool == NULL) {
    pool->forw = pool;
    pool->back = pool;
    thread_pool = pool;
    } else {
        thread_pool->back->forw = pool;
        pool->forw = thread_pool;
        pool->back = thread_pool->back;
        thread_pool->back = pool;
    }
    pthread_mutex_unlock(&thread_pool_lock);

    return pool;

}


/***
 * brief: 往线程池中添加任务
 * @param1 [in] pThreadPool_t *pool  线程池
 * @param2 [in] task_callback_t  添加任务的回调函数
 * @param3 [in] arg 回调函数的参数
 * return -1  失败， return 0成功
***/
int ThreadPoolQueue(pThreadPool_t *pool, task_callback_t func, void *arg) {
    task_t *task = (task_t*)malloc(sizeof(task_t));
    if (task == NULL) {
        errno = ENOMEM;
        return -1;
    }

    task->next = NULL;
    task->func = func;
    task->arg = arg;

    pthread_mutex_lock(&pool->mtx);

    if (pool->head == NULL) {
        pool->head = task;
    } else {
        pool->tail->next = task;
    }
    pool->tail = task;

    // 若线程池中的线程有空闲的，就激活，若没有空闲，且没有到线程池设置的最大线程数，就创建线程
    if (pool->idle > 0) {
        pthread_cond_signal(&pool->workcv);
    } else if (pool->nthreads < pool->maximum && WorkerCreate(pool) == 0) {
        pool->nthreads ++;
        printf("%s pool->nthreads==%d\n",__func__, pool->nthreads);
    }

    pthread_mutex_unlock(&pool->mtx);
    return 0;

}


void pThreadPool_tWait(pThreadPool_t *pool) {

    pthread_mutex_lock(&pool->mtx);
    pthread_cleanup_push(pthread_mutex_unlock, (void *)&pool->mtx);

    while (pool->head != NULL || pool->idle != pool->nthreads) {
        pool->flags |= POOL_WAIT;
        pthread_cond_wait(&pool->waitcv, &pool->mtx);
    }

    pthread_cleanup_pop(1);
}


void pThreadPool_tDestroy(pThreadPool_t *pool) {

    worker_t *activep;
    task_t *task;

    pthread_mutex_lock(&pool->mtx);
    pthread_cleanup_push(pthread_mutex_unlock, (void *)&pool->mtx);
    pool->flags |= POOL_DESTROY;
    pthread_cleanup_pop(1);

    pthread_cond_broadcast(&pool->workcv);
    for (activep = pool->active;activep != NULL;activep = activep->active_next) {

        printf("activep->active_tid == %lld\n", activep->active_tid);
        pthread_cancel(activep->active_tid);
    }

    pthread_mutex_lock(&pool->mtx);
    pthread_cleanup_push(pthread_mutex_unlock, (void *)&pool->mtx);
    while (pool->nthreads != 0) {
#if 1
        pthread_cond_broadcast(&pool->workcv);
        printf("%s pthread_cond_broadcast\n",__func__);
        pthread_cond_wait(&pool->busycv, &pool->mtx);
#else
        printf("%s pthread_cond_broadcast\n",__func__);
        sleep(2);
        pthread_cond_broadcast(&pool->workcv);
#endif
    }
    pthread_cleanup_pop(1);

    pthread_mutex_lock(&thread_pool_lock);
    if (thread_pool == pool) {
        thread_pool = pool->forw;
    }
    if (thread_pool == pool) {
        thread_pool = NULL;
    } else {
        pool->back->forw = pool->forw;
        pool->forw->back = pool->back;
    }
    pthread_mutex_unlock(&thread_pool_lock);

    for (task = pool->head;task != NULL;task = pool->head) {
        pool->head = task->next;
        free(task);
    }

    pthread_attr_destroy(&pool->attr);
    free(pool);
}


/********************************* debug thread pool *********************************/

void king_counter(void *arg) {

    int index = *(int*)arg;
#if 1
    printf("index : %d, selfid : %lld\n", index, pthread_self());
#endif
    free(arg);
    usleep(1);
}


#define KING_COUNTER_SIZE 100

int main(int argc, char *argv[]) {

    pThreadPool_t *pool = ThreadPoolCreate(10, 20, 15, NULL);

    int i = 0;
    for (i = 0;i < KING_COUNTER_SIZE;i ++) {
        int *index = (int*)malloc(sizeof(int));

        memset(index, 0, sizeof(int));
        memcpy(index, &i, sizeof(int));

        ThreadPoolQueue(pool, king_counter, index);
    }

    //usleep(100);
    sleep(2);
    //pThreadPool_tWait(pool);
    printf("pool->nthreads==%d\n", pool->nthreads);
    pThreadPool_tDestroy(pool);
    printf("You are very good !!!!\n");
}



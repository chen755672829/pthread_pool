#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

// 日志打印暂未完善
//#define threadpool_printf(data)  

#define POST_LEN    65536

// 默认线程队列中最大缓存任务数量
#define  THREAD_POOL_QUEUE_DEFAULT_MAX_LEN   65536
typedef void (*thread_task_callback_t)(void *);
typedef pthread_mutex_t  pthread_mutex_t;
typedef pthread_cond_t  pthread_cond_t;
typedef unsigned char  uchar;
typedef unsigned int uint_32;

#define EVENT_NOTIFY  0

typedef struct {
    int      len;
    u_char     *data;
} str_t;

typedef struct thread_task_s {
    struct thread_task_s *next;
    thread_task_callback_t handler;
    void *arg;
    // 事件通知，这里暂时不写。
}thread_task_t;

typedef struct {
    thread_task_t        *first;
    thread_task_t       **last;
} thread_pool_queue_t;

#define thread_pool_queue_init(q)                                         \
    (q)->first = NULL;                                                        \
    (q)->last = &(q)->first

typedef struct thread_pool_s {
    struct thread_pool_s *forw;
    struct thread_pool_s *back;
    pthread_mutex_t        mtx;          // 互斥锁
    pthread_cond_t         cond;         // 条件变量
    thread_pool_queue_t   queue;        // 任务队列
    int                 waiting;      //有多少个任务正在等待处理
    str_t                 name;         // 线程池名
    uint_32                threads;      // 当前的线程数
    int                 max_queue;    // 线程池最大能处理的任务数

    pthread_attr_t          pthread_attr;
    //ngx_log_t                *log;          // 日志-> 暂时不做
    //u_char                   *file;         // 配置文件名
    //ngx_uint_t                line;         // thread_pool配置在配置文件中的行号
} thread_pool_t;

static pthread_mutex_t thread_pool_muxlock = PTHREAD_MUTEX_INITIALIZER;
thread_pool_t *pthread_pool = NULL;

/***
 * nginx中是在配置文件中进行配置，这里暂时使用函数创建
 * breif: 根据参数创建线程池
 * @param1 [in] uint_32 nthreads  创建的线程数
 * @param2 [in] uchar *name  线程池名字
 * @param3 [in] int namelen  线程池名字长度
 * @param4 [in] pthread_attr_t *attr 创建的线程属性。
 * return thread_pool_t *  线程池结构指针
***/
thread_pool_t * pthread_pool_create(uint_32 nthreads, uchar *name, int namelen, pthread_attr_t *attr);
/***
 * breif: 销毁线程池
 * @param1 [in] thread_pool_t *tp 要销毁的线程池
***/
static void pthread_pool_destroy(thread_pool_t *tp);

/***
 * breif: ①初始化线程池的任务队列 ②设置初始化互斥锁和条件变量 ③根据设置线程池的线程数，创建线程。
 * @param1[in] thread_pool_t *tp 线程池
 *
***/ 
static int pthread_pool_init(thread_pool_t *tp);

/***
 * breif: 结束线程任务的回调函数，作用：结束当前线程。
 * @param1[in] void *data  回调函数的参数
 *
***/ 
static void pthread_pool_exit_handler(void *data);

/***
 * breif: 启动新线程的运行函数
 * @param1[in] void *data  使用pthread_create新建线程时的传参。
 *
***/ 
static void *pthread_pool_cycle(void *data);

/* nginx 通知函数-> 当时任务执行完后事件通知函数 */
/* 目前不清楚，nginx的事件通知 */
/* nginx 的经典是线程通知那一块 */
// static void pthread_pool_handler(ngx_event_t *ev);


static void *
pthread_pool_cycle(void *data)
{
    thread_pool_t *tp = data;
    sigset_t            set;
    thread_task_t *task;
    int err;

    sigfillset(&set);
    sigdelset(&set, SIGILL);
    sigdelset(&set, SIGFPE);
    sigdelset(&set, SIGSEGV);
    sigdelset(&set, SIGBUS);
    err = pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (err) {
        return NULL;
    }

    for ( ;; ) {
        if(pthread_mutex_lock(&tp->mtx) != 0) {
            return NULL;
        }
        tp->waiting--;
        while (tp->queue.first == NULL) {
            if (pthread_cond_wait(&tp->cond, &tp->mtx) != 0) {
                pthread_mutex_unlock(&tp->mtx);
                return NULL;
            }
        }

        task = tp->queue.first;
        tp->queue.first = task->next;
        // 维护 tp->queue.last 双指针。
        if (tp->queue.first == NULL) {
            tp->queue.last = &tp->queue.first;
        }

        if (pthread_mutex_unlock(&tp->mtx) != 0) {
            return NULL;
        }
        task->handler(task->arg);

// 事件通知
#if EVENT_NOTIFY
        
#else
        free(task);
#endif
    }
}


int
pthread_task_post(thread_pool_t *tp, thread_task_t *task)
{
    if (pthread_mutex_lock(&tp->mtx) != 0) {
        return -1;
    }

    if (tp->waiting >= tp->max_queue) {
        printf("%s tp->waiting==%d queue fill!\n", __func__, tp->waiting);
        pthread_mutex_unlock(&tp->mtx);
        return -1;
    }

    if (pthread_cond_signal(&tp->cond) != 0) {
        pthread_mutex_unlock(&tp->mtx);
        return -1;
     }
#if EVENT_NOTIFY
    
#endif
    *tp->queue.last = task;
    task->next = NULL;
    tp->queue.last = &task->next;

    tp->waiting++;

    pthread_mutex_unlock(&tp->mtx);
    return 0;

}


int
pthread_cond_create(pthread_cond_t *cond)
{
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
        goto cond_attr_destroy;
    }

    err = pthread_cond_init(cond, &attr);
    if (err != 0) {
        goto cond_attr_destroy;
    }

    err = pthread_condattr_destroy(&attr);
    if (err != 0) {
        return -1;
    }

    return 0;

cond_attr_destroy:
    pthread_condattr_destroy(&attr);
    return -1;
}


int
pthread_mutex_create(pthread_mutex_t *mtx)
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
        goto mutex_attr_destroy;
    }

    err = pthread_mutex_init(mtx, &attr);
    if (err != 0) {
       goto mutex_attr_destroy;
    }
    err = pthread_mutexattr_destroy(&attr);
    if (err != 0) {
        return -1;
    }
    return 0;

mutex_attr_destroy:
    pthread_mutexattr_destroy(&attr);
    return -1;
}


static void 
pCloneAttributes(pthread_attr_t *new_attr, pthread_attr_t *old_attr) {

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


static int 
pthread_pool_init(thread_pool_t *tp)
{
    int n, err;
    pthread_t       tid;
    thread_pool_queue_init(&tp->queue);
    if (pthread_mutex_create(&tp->mtx) != 0) {
        return -1;
    }

    if (pthread_cond_create(&tp->cond) != 0) {
        goto destroy_mutex;
    }

    for (n = 0; n < tp->threads; n++) {
        err = pthread_create(&tid, &tp->pthread_attr, pthread_pool_cycle, tp);
        if (err) {
            //printf("")
            goto destroy_cond;
        }
    }

    return 0;

destroy_cond:
    pthread_cond_destroy;
destroy_mutex:
    pthread_mutex_destroy(&tp->mtx);
    return -1;
}


thread_pool_t * 
pthread_pool_create(uint_32 nthreads, uchar *name, int namelen, pthread_attr_t *attr)
{
    if (nthreads <= 0) {
        // 日志打印先不搞了
        //printf("  %s, nthreads == %d thread pool create fail!\n", __func__, )
        return NULL;
    }
    thread_pool_t *thpool_ptr;
    thpool_ptr = malloc(sizeof(thread_pool_t));
    if (thpool_ptr == NULL) {
        return NULL;
    }
    memset(thpool_ptr,0,sizeof(thread_pool_t));

    thpool_ptr->waiting = 0;
    thpool_ptr->threads= nthreads;
    thpool_ptr->max_queue = THREAD_POOL_QUEUE_DEFAULT_MAX_LEN;

    if (name==NULL || namelen <= 0) {
        //printf();
        goto free_thread_pool;
    }

    thpool_ptr->name.data = malloc(namelen+1);
    if (thpool_ptr->name.data == NULL) {
        // printf();
        goto free_thread_pool;
    }

    memcpy(thpool_ptr->name.data, name, namelen);
    *(thpool_ptr->name.data + namelen + 1) = '\0';
    thpool_ptr->name.len = namelen;

    pCloneAttributes(&thpool_ptr->pthread_attr, attr);

    if (pthread_pool_init(thpool_ptr) != 0) {
        goto free_pool_name;
    }

    pthread_mutex_lock(&thread_pool_muxlock);

    if (pthread_pool == NULL) {
        thpool_ptr->forw = thpool_ptr;
        thpool_ptr->back = thpool_ptr;
        pthread_pool = thpool_ptr;

    } else {
        pthread_pool->back->forw = thpool_ptr;
        thpool_ptr->forw = pthread_pool;
        thpool_ptr->back = pthread_pool->back;
        pthread_pool->back = thpool_ptr;
    }
    pthread_mutex_unlock(&thread_pool_muxlock);
    return thpool_ptr;

free_pool_name:
    free(thpool_ptr->name.data);
free_thread_pool:
    free(thpool_ptr);
    return NULL;

}


static void
pthread_pool_exit_handler(void *data)
{
    int *lock = data;
    *lock = 0;
    printf("%s pthread_exit**********\n", __func__);
    pthread_exit(0);
}


static void
pthread_pool_destroy(thread_pool_t *tp)
{
    volatile int  lock;
    int n;
    thread_task_t * task;
    for (n = 0; n < tp->threads; n++) {
        lock = 1;
        task = malloc(sizeof(thread_task_t));
        task->handler = pthread_pool_exit_handler;
        task->arg = (void*)&lock;

        if (pthread_task_post(tp, task) != 0) {
            // 打印日志
            return ;
        }

        while (lock) {
            //printf("%s", __func__)
            sched_yield();
        }

    }

    pthread_mutex_lock(&thread_pool_muxlock);

    if (pthread_pool == tp) {
        pthread_pool = tp->forw;
    }

    if (pthread_pool == tp) {
        pthread_pool = NULL;
    } else {
        tp->back->forw = tp->forw;
        tp->forw->back = tp->back;
    }

    pthread_mutex_unlock(&thread_pool_muxlock);

    pthread_cond_destroy(&tp->cond);
    pthread_mutex_destroy(&tp->mtx);

    free(tp->name.data);
    free(tp);

}


void 
pthread_task_callback(void * arg)
{
    int *index = (int *)arg;
    printf("%s arg==%d\n",__func__, *index);
    
    //usleep(1);
    int i;
#if 0
    for (i = 0;i < 5; i++) {
        printf("%s arg==%d\n",__func__, *index);
    }
#endif

    free(index);
}


int main()
{
    char name[] = "thead_pool";
    int len = strlen(name);
    thread_pool_t *tp = pthread_pool_create(4, name, len, NULL);
    int i;
    thread_task_t *task = NULL;
    for (i = 0; i < POST_LEN; i++) {
        int *index = malloc(sizeof(int));
        *index = i;
        task = malloc(sizeof(thread_task_t));
        task->handler = pthread_task_callback;
        task->arg = (void*)index;
        if (pthread_task_post(tp, task) != 0) {
            printf("%s index==%d post fail!\n",__func__, *index);
            break;
        }
    }

    pthread_pool_destroy(tp);
    getchar();
    return 0;
}




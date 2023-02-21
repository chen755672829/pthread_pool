# pthread_pool

## pthread_pool_nginx_simple.c 线程池
  根据nginx线程池改编：  
  - 优点: 比较稳定，可以设置线程属性。   
  - 缺点：目前没有实现nginx中的执行完当前任务后的任务通知事件的操作，线程数，不能随之改变。  
  线程池详解文档：https://note.youdao.com/s/CqFFkqpi  

![image](https://user-images.githubusercontent.com/35031390/202134321-ad540042-1cc4-4c67-92ca-33593cfde0b8.png)

![image](https://user-images.githubusercontent.com/35031390/202135718-9da47afe-42ed-45ab-808f-e0d5cfa01a92.png)

## pthreadpool.c  线程池：
  这个线程池的优点：  
  - 可以设置最大线程数和最小线程数，且能设置线程超时时间，当超时时间内，没有任务到来，那么线程池中线程数将变为最小线程数。  
  - 可以设置线程属性  
    
  这个线程池详解：https://note.youdao.com/s/bJA7lXpP  
  这个线程池存在bug:  
  因使用pthread_cancel() 进行销毁函数，其中也配合设置了线程属性，但是还是有时间销毁线程时，有些线程无法销毁，导致阻塞。查看线程如下，目前我无法定位根因，怎么更改代码，请大佬更正。 
```
[root@lvs pthread_pool]# pstree -p `pidof pthreadpool`
pthreadpool(37250)─┬─{pthreadpool}(37262)
               └─{pthreadpool}(37263)
[root@lvs pthread_pool]# pstack 37262
Thread 1 (process 37262):
#0  0x00007f89c377554d in __lll_lock_wait () from /lib64/libpthread.so.0
#1  0x00007f89c3777d3c in _L_cond_lock_847 () from /lib64/libpthread.so.0
#2  0x00007f89c3777bd1 in __pthread_mutex_cond_lock () from /lib64/libpthread.so.0
#3  0x00007f89c3772c9b in __condvar_cleanup1 () from /lib64/libpthread.so.0
#4  0x0000000000401a69 in WorkerThread ()
#5  0x00007f89c376eea5 in start_thread () from /lib64/libpthread.so.0
#6  0x00007f89c34978dd in clone () from /lib64/libc.so.6
[root@lvs pthread_pool]#
[root@lvs pthread_pool]#
[root@lvs pthread_pool]# pstack 37263
'Thread 1 (process 37263):
#0  0x00007f89c377554d in __lll_lock_wait () from /lib64/libpthread.so.0
#1  0x00007f89c3777d3c in _L_cond_lock_847 () from /lib64/libpthread.so.0
#2  0x00007f89c3777bd1 in __pthread_mutex_cond_lock () from /lib64/libpthread.so.0
#3  0x00007f89c3772c9b in __condvar_cleanup1 () from /lib64/libpthread.so.0
#4  0x0000000000401a69 in WorkerThread ()
#5  0x00007f89c376eea5 in start_thread () from /lib64/libpthread.so.0
#6  0x00007f89c34978dd in clone () from /lib64/libc.so.6
```

# pthread_pool

pthread_pool_nginx_simple.c 线程池
    根据 nginx改变，优点: 比较稳定。 缺点：目前没有实现nginx中的执行完当前任务后的任务通知事件的操作，线程数，不能随之改变。
    线程池详解文档：https://note.youdao.com/s/CqFFkqpi

![image](https://user-images.githubusercontent.com/35031390/202134321-ad540042-1cc4-4c67-92ca-33593cfde0b8.png)

![image](https://user-images.githubusercontent.com/35031390/202135718-9da47afe-42ed-45ab-808f-e0d5cfa01a92.png)

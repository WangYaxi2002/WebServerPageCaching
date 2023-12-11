#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <time.h>
#include <sys/time.h>
#define VERSION 23
#define BUFSIZE 8096
#define ERROR 42
#define LOG 44
#define FORBIDDEN 403
#define NOTFOUND 404
#ifndef SIGCLD
#define SIGCLD SIGCHLD
#endif
struct
{
    char* ext;
    char* filetype;
} extensions[] = {
    {"gif", "image/gif"},
    {"jpg", "image/jpg"},
    {"jpeg", "image/jpeg"},
    {"png", "image/png"},
    {"ico", "image/ico"},
    {"zip", "image/zip"},
    {"gz", "image/gz"},
    {"tar", "image/tar"},
    {"htm", "text/html"},
    {"html", "text/html"},
    {0, 0} };

/* queue status and conditional variable*/
typedef struct staconv
{
    pthread_mutex_t mutex;
    pthread_cond_t cond; /*用于阻塞和唤醒线程池中的线程*/
    int status;          /*表示任务队列状态： false 表示无任务； true 表示有任务*/
} staconv;

/*Task*/
typedef struct task
{
    struct task* next;           /* 指向下一个任务 */
    void (*function)(void* arg); /* 函数指针 */
    void* arg;                   /* 函数参数指针 */
} task;

/*Task Queue*/
typedef struct taskqueue
{
    pthread_mutex_t mutex; /* 用于互斥锁/写任务队列? */
    task* front;           /* 指向队首 */
    task* rear;            /* 指向队尾 */
    staconv* has_jobs;     /* 根据状态，阻塞线程 */
    int len;               /* 队列中任务个数 */
} taskqueue;

/* Thread */
typedef struct thread
{
    int id;                  /* 线程 id */
    pthread_t pthread;       /* 封装的 POSIX 线程 */
    struct threadpool* pool; /* 与线程池绑定 */
} thread;

/*Thread Pool*/
typedef struct threadpool
{
    thread** threads;                /* 线程指针数组 */
    volatile int num_threads;        /* 线程池中线程数量 */
    volatile int num_working;        /* 目前正在工作的线程个数*/
    pthread_mutex_t thcount_lock;    /* 线程池锁 用于修改上面两个变量 */
    pthread_cond_t threads_all_idle; /* 用于销毁线程的条件变量 */
    taskqueue queue;                 /* 任务队列 */
    volatile int is_alive;           /* 表示线程池是否还存活 */
    int max_num;                     /* 线程最高活跃数量 */
    int min_num;                     /* 最低活跃数量 */
    double active, block;            /*线程平均活跃时间及其阻塞时间*/
} threadpool;

void error(char* s)
{
    printf("%s\n", s);
}

void logger(int type, char* s1, char* s2, int socket_fd)
{
    int fd;
    char logbuffer[BUFSIZE * 2];
    time_t now_time;
    time(&now_time);
    switch (type)
    {
    case ERROR:
        (void)sprintf(logbuffer, "TIME:%s ERROR: %s:%s Errno=%d exiting pid=%d", ctime(&now_time), s1, s2, errno, getpid());
        break;
    case FORBIDDEN:
        (void)write(socket_fd, "HTTP/1.1 403 FORBIDDEN\nContent-Length: 185\nConnection:close\nContent-Type:text/html\n\n<html><head>\n<title>403FORBIDDEN</title>\n<head><body>\n<h1>FORBIDDEN</h1>\nThe requested URL, file type or operation isnot allowed on this simple static file webserver.\n</body></html>\n", 267);
        (void)sprintf(logbuffer, "TIME:%s FORBIDDEN: %s:%s,%d", ctime(&now_time), s1, s2, socket_fd);
        break;
    case NOTFOUND:
        (void)write(socket_fd, "HTTP/1.1 404 Not Found\nContent-Length: 136\nConnection:close\nContent-Type: text/html\n\n<html><head>\n<title>404 NotFound</title>\n</head><body>\n<h1>Not Found</h1>\nThe requested URL was not found on thisserver.\n</body></html>\n", 222);
        (void)sprintf(logbuffer, "TIME:%s NOT FOUND: %s:%s", ctime(&now_time), s1, s2);
        break;
    case LOG:
        (void)sprintf(logbuffer, "INFO: %s:%s:%d TIME:%s", s1, s2, socket_fd, ctime(&now_time));
        break;
    }
    /* No checks here, nothing can be done with a failure anyway */
    if ((fd = open("nweb.log", O_CREAT | O_WRONLY | O_APPEND, 0644)) >= 0)
    {
        (void)write(fd, logbuffer, strlen(logbuffer));
        (void)write(fd, "\n", 1);
        (void)close(fd);
    }
    // if(type == ERROR || type == NOTFOUND || type == FORBIDDEN) exit(3);
}

// 初始化任务队列
void init_taskqueue(taskqueue* queue)
{
    pthread_mutex_init(&queue->mutex, NULL);              /*初始化互斥锁*/
    queue->front = NULL;                                  /*初始化队首指针*/
    queue->rear = NULL;                                   /*初始化队尾指针*/
    queue->has_jobs = (staconv*)malloc(sizeof(staconv)); /*初始化staconv指针*/
    pthread_mutex_init(&(queue->has_jobs->mutex), NULL);  /*初始化互斥锁*/
    pthread_cond_init(&queue->has_jobs->cond, NULL);      /*初始化条件变量*/
    queue->has_jobs->status = 0;                          /*设置初始队列中无任务*/
    queue->len = 0;                                       /*初始化任务个数*/
}

// 将任务加入队列
void push_taskqueue(taskqueue* queue, task* curtask)
{
    curtask->next = NULL; /*插入队尾，下一个任务为空*/
    pthread_mutex_lock(&queue->mutex);
    if (queue->len == 0)
    {
        queue->front = curtask;
    }
    else
    {
        queue->rear->next = curtask;
    }
    queue->rear = curtask;
    queue->len++;
    if (queue->has_jobs->status == 0)
    {
        queue->has_jobs->status = 1;
    }
    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_signal(&queue->has_jobs->cond);
}

// 销毁任务队列
void destory_taskqueue(taskqueue* queue)
{
    pthread_cond_destroy(&queue->has_jobs->cond);

    pthread_mutex_destroy(&queue->mutex);
    free(&queue->mutex);
    pthread_mutex_destroy(&queue->has_jobs->mutex);
    free(&queue->has_jobs->mutex);
}

// take_taskqueue 从任务队列头部提取任务，并在队列中删除此任务
task* take_taskqueue(taskqueue* queue)
{
    task* r = NULL;
    pthread_mutex_lock(&queue->mutex);
    if (queue->len > 0 && queue->front != NULL)
    {
        r = queue->front;
        queue->front = r->next;
        queue->len--;
        if (queue->len == 0 || r->next == NULL)
        {
            queue->has_jobs->status = 0;
        }
    }
    pthread_mutex_unlock(&queue->mutex);
    return r;
}

/*等待当前任务全部执行完*/
void waitThreadPool(threadpool* pool)
{
    pthread_mutex_lock(&pool->thcount_lock);
    while (pool->queue.len || pool->num_working)
    {
        pthread_cond_wait(&pool->threads_all_idle, &pool->thcount_lock);
    }
    pthread_mutex_unlock(&pool->thcount_lock);
}

/*线程运行的逻辑函数*/
void* thread_do(void* pthread0)
{
    struct timeval startA, endA, startB, endB;
    struct thread* pthread = (struct thread*)pthread0;
    /* 设置线程名字 */
    char thread_name[128] = { 0 };
    sprintf(thread_name, "thread-pool-%d", pthread->id);
    prctl(PR_SET_NAME, thread_name);
    /* 获得线程池*/
    threadpool* pool = pthread->pool;
    /* 在线程池初始化时，对已经创建线程的计数，执行 pool->num_threads++ */
    pthread_mutex_lock(&pool->thcount_lock);
    pool->num_threads++;
    pthread_mutex_unlock(&pool->thcount_lock);
    /*线程一直循环运行，直到 pool->is_alive 变为 false*/

    while (pool->is_alive)
    {
        /*如果任务队列中还有任务，则继续运行，否则阻塞*/
        gettimeofday(&startB, NULL);
        pthread_mutex_lock(&pool->queue.has_jobs->mutex);
        while (pool->queue.has_jobs->status == 0 || pool->queue.len == 0)
        {
            pthread_cond_wait(&pool->queue.has_jobs->cond, &pool->queue.has_jobs->mutex);
        }
        pthread_mutex_unlock(&pool->queue.has_jobs->mutex);
        gettimeofday(&endB, NULL);
        if (pool->is_alive)
        {
            gettimeofday(&startA, NULL);
            /*执行到此位置，表明线程在工作，需要对工作线程数量进行计数*/
            pthread_mutex_lock(&pool->thcount_lock);
            pool->block += (double)((endB.tv_sec - startB.tv_sec) + ((endB.tv_usec - startB.tv_usec) / 1000000.0));
            pool->num_working++;
            if (pool->num_working > pool->max_num)
            {
                pool->max_num = pool->num_working;
            }
            pthread_mutex_unlock(&pool->thcount_lock);
            /* 从任务队列的队首提取任务，并执行该任务*/
            task* curtask = take_taskqueue(&pool->queue);
            void (*func)(void*);
            void* arg;
            // take_taskqueue 用于从任务队列头部提取任务，并在队列中删除此任务
            if (curtask)
            {
                func = curtask->function;
                arg = curtask->arg;
                // 执行任务
                func(arg);
                // 释放任务
                free(curtask);
            }
            /*执行到此位置，表明线程已经将任务执行完毕，需改变工作线程数量*/
            int cnt;
            gettimeofday(&endA, NULL);
            pthread_mutex_lock(&pool->thcount_lock);
            pool->active += (double)((endA.tv_sec - startA.tv_sec) + ((endA.tv_usec - startA.tv_usec) / 1000000.0));
            pool->num_working--;
            if (pool->num_working < pool->min_num)
            {
                pool->min_num = pool->num_working;
            }
            cnt = pool->num_working;
            pthread_mutex_unlock(&pool->thcount_lock);
            // 从此处还需注意，当工作线程数量为 0时，表示任务全部完成，会使阻塞在 waitThreadPool 函数上的线程继续运行
            if (cnt == 0)
            {
                pthread_cond_signal(&pool->threads_all_idle);
            }
        }
        else
        {
            pthread_mutex_lock(&pool->thcount_lock);
            pool->block += (double)((endB.tv_sec - startB.tv_sec) + ((endB.tv_usec - startB.tv_usec) / 1000000.0));
            pthread_mutex_unlock(&pool->thcount_lock);
        }
    }
    /*运行到此位置表明线程将要退出，需改变当前线程池中的线程数*/
    pthread_mutex_lock(&pool->thcount_lock);
    pool->num_threads--;
    pthread_mutex_unlock(&pool->thcount_lock);
}

/*创建线程*/
int create_thread(struct threadpool* pool, struct thread** pthread, int id)
{
    // 为thread 分配内存空间
    *pthread = (struct thread*)malloc(sizeof(struct thread));
    if (pthread == NULL)
    {
        error("creat_thread(): Could not allocate memory for thread\n");
        return -1;
    }
    // 设置这个 thread 的属性
    (*pthread)->pool = pool;
    (*pthread)->id = id;
    // 创建线程
    pthread_create(&(*pthread)->pthread, NULL, thread_do, (*pthread));
    pthread_detach((*pthread)->pthread);
    return 0;
}

/*线程池初始化函数*/
struct threadpool* initTheadPool(int num_threads)
{
    // 创建线程池
    threadpool* pool;
    pool = (threadpool*)malloc(sizeof(struct threadpool));
    pool->block = 0;
    pool->active = 0;
    pool->max_num = 0;
    pool->min_num = num_threads;
    pool->num_threads = 0;
    pool->num_working = 0;
    pool->is_alive = 1;
    // 初始化互斥量和条件变量
    pthread_mutex_init(&pool->thcount_lock, NULL);
    pthread_cond_init(&pool->threads_all_idle, NULL);
    // 初始化任务队列
    init_taskqueue(&pool->queue);
    // 创建线程数组
    pool->threads = (struct thread**)malloc(num_threads * sizeof(struct thread));
    // 创建线程
    for (int i = 0; i < num_threads; ++i)
    {
        create_thread(pool, pool->threads + i, i); // i 为线程 id,
    }
    // 待所有的线程创建完毕,在每个线程运行函数中将进行 pool->num_threads++ 操作
    // 因此，此处为忙等待，直到所有的线程创建完毕，并在马上运行阻塞代码时才返回
    while (pool->num_threads != num_threads)
    {
    }
    return pool;
}

/*向线程池中添加任务*/
void addTask2ThreadPool(threadpool* pool, task* curtask)
{
    // 将任务加入队列
    push_taskqueue(&pool->queue, curtask);
}

/*销毁线程池*/
void destoryThreadPool(threadpool* pool)
{
    // 如果当前任务队列中有任务，需等待任务队列为空，并且运行线程以执行完任务
    waitThreadPool(pool);
    pool->is_alive = 0;
    // 销毁任务队列
    destory_taskqueue(&pool->queue);
    // 销毁线程指针数,并释放所有为线程池分配的内存
    for (int i = 0; i < pool->num_threads; i++)
    {
        free(pool->threads[i]);
    }
    free(pool);
}

/*获得当前线程池中正在运行线程的数*/
int getNumofThreadWorking(threadpool* pool)
{
    return pool->num_working;
}

unsigned long get_file_size(const char* path)
{
    unsigned long filesize = -1;
    struct stat statbuff;
    if (stat(path, &statbuff) < 0)
    {
        return filesize;
    }
    else
    {
        filesize = statbuff.st_size;
    }
    return filesize;
}

typedef struct
{
    int fd;                  // 通道号
    int hit;                 // 次数，在发送消息中代表buffer长度
    char* buffer;            // 缓冲区
    threadpool* rftp, * smtp; // 不同任务的线程池
} webparam;

/*发送消息*/
void sendMsg(void* data)
{
    webparam* param = (webparam*)data;
    (void)write(param->fd, param->buffer, param->hit);
    free(param->buffer);
    usleep(500);
    close(param->fd);
    free(param);
}

/*读文件*/
void readFile(void* data)
{
    int file_fd, buflen;
    char* fstr;
    long i, len;
    int ret;
    webparam* param = (webparam*)data;
    buflen = strlen(param->buffer);
    fstr = (char*)0;
    for (i = 0; extensions[i].ext != 0; i++)
    {
        len = strlen(extensions[i].ext);
        if (!strncmp(&param->buffer[buflen - len], extensions[i].ext, len))
        {
            fstr = extensions[i].filetype;
            break;
        }
    }
    if (fstr == 0)
        logger(FORBIDDEN, "file extension type not supported", param->buffer, param->fd);
    if ((file_fd = open(&param->buffer[5], O_RDONLY)) == -1)
    {
        /* open the file for reading */
        logger(NOTFOUND, "failed to open file", &param->buffer[5], param->fd);
    }
    logger(LOG, "send", &param->buffer[5], param->hit);
    len = (long)lseek(file_fd, (off_t)0, SEEK_END);
    (void)lseek(file_fd, (off_t)0, SEEK_SET);
    (void)sprintf(param->buffer, "HTTP/1.1 200 OK\nServer: nweb/%d.0\nContent-Length: %ld\nConnection:close\nContent-Type: %s\n\n", VERSION, len, fstr); /* header + a blank line */
    logger(LOG, "header", param->buffer, param->hit);
    webparam* sm = (webparam*)malloc(sizeof(webparam));
    sm->fd = param->fd;
    sm->buffer = (char*)malloc(sizeof(char) * (len + BUFSIZE));
    strcpy(sm->buffer, param->buffer);
    ret = read(file_fd, sm->buffer + strlen(param->buffer), len);
    sm->hit = ret + strlen(param->buffer);
    task* t = (task*)malloc(sizeof(task));
    t->arg = sm;
    t->function = sendMsg;
    addTask2ThreadPool(param->smtp, t); /*添加发送信息的任务*/
    close(file_fd);
}

/*从客户端socket通道中读取消息并对其进行解析*/
void readMsg(void* data)
{
    int j, buflen;
    long i, ret, len;
    webparam* param = (webparam*)data;
    ret = read(param->fd, param->buffer, BUFSIZE); /* read web request in one go */
    if (ret == 0 || ret == -1)
    {
        logger(FORBIDDEN, "failed to read browser request", "", param->fd);
    }
    else
    {
        if (ret > 0 && ret < BUFSIZE) /* 返回码是有效字符 */
            param->buffer[ret] = 0;   /* 终止缓冲区 */
        else
            param->buffer[0] = 0;
        for (i = 0; i < ret; i++) /* remove cf and lf characters */
            if (param->buffer[i] == '\r' || param->buffer[i] == '\n')
                param->buffer[i] = '*';
        logger(LOG, "request", param->buffer, param->hit);                          /*写请求日志*/
        if (strncmp(param->buffer, "GET ", 4) && strncmp(param->buffer, "get ", 4)) /*不是get请求*/
        {
            logger(FORBIDDEN, "only simple get operation supported", param->buffer, param->fd);
        }
        for (i = 4; i < BUFSIZE; i++) /*在第二个空格后终止，忽略额外的内容*/
        {                             /* null terminate after the second space to ignore extra stuff */
            if (param->buffer[i] == ' ')
            { /* string is "get url " +lots of other stuff */
                param->buffer[i] = 0;
                break;
            }
        }
        for (j = 0; j < i - 1; j++) /* 检查是否非法使用父目录 .. */
            if (param->buffer[j] == '.' && param->buffer[j + 1] == '.')
            {
                logger(FORBIDDEN, "parent directory (..) path names not supported", param->buffer, param->fd);
            }
        if (!strncmp(&param->buffer[0], "get /\0", 6) || !strncmp(&param->buffer[0], "get /\0", 6)) /* 没有文件名转换为索引文件 */
            (void)strcpy(param->buffer, "get /index.html");
        task* t = (task*)malloc(sizeof(task));
        t->arg = param;
        t->function = readFile;
        addTask2ThreadPool(param->rftp, t); /*添加读文件任务*/
    }
}

typedef struct
{
    threadpool* p1, * p2, * p3;
    int num1, num2, num3;
} monitorFP;

void* monitor(void* data)
{
    monitorFP* param = (monitorFP*)data;
    int fd;
    char logbuffer[BUFSIZE];
    int max_num, min_num, avg_mun = 0, len;
    double atime, btime;
    while (1)
    {
        sleep(2);
        pthread_mutex_lock(&param->p1->thcount_lock);
        len = param->p1->queue.len;
        max_num = param->p1->max_num;
        min_num = param->p1->min_num;
        atime = param->p1->active / param->num1 / 1000;
        btime = param->p1->block / param->num1 / 1000;
        pthread_mutex_unlock(&param->p1->thcount_lock);
        sprintf(logbuffer, "线程池中线程平均活跃时间:%lf\t阻塞时间:%lf\n线程最高活跃数量:%d\t最低活跃数量:%d\t平均活跃数量:%d\n消息队列中消息的长度:%d\n", atime, btime, max_num, min_num, avg_mun, len);
        if ((fd = open("monitor.log", O_CREAT | O_WRONLY | O_APPEND, 0644)) >= 0)
        {
            (void)write(fd, logbuffer, strlen(logbuffer));
            (void)write(fd, "\n", 1);
            (void)close(fd);
        }
        pthread_mutex_lock(&param->p2->thcount_lock);
        len = param->p2->queue.len;
        max_num = param->p2->max_num;
        min_num = param->p2->min_num;
        atime = param->p2->active / param->num2 / 1000;
        btime = param->p2->block / param->num2 / 1000;
        pthread_mutex_unlock(&param->p2->thcount_lock);
        sprintf(logbuffer, "线程池中线程平均活跃时间:%lf\t阻塞时间:%lf\n线程最高活跃数量:%d\t最低活跃数量:%d\t平均活跃数量:%d\n消息队列中消息的长度:%d\n", atime, btime, max_num, min_num, avg_mun, len);
        if ((fd = open("monitor.log", O_CREAT | O_WRONLY | O_APPEND, 0644)) >= 0)
        {
            (void)write(fd, logbuffer, strlen(logbuffer));
            (void)write(fd, "\n", 1);
            (void)close(fd);
        }
        pthread_mutex_lock(&param->p3->thcount_lock);
        len = param->p3->queue.len;
        max_num = param->p3->max_num;
        min_num = param->p3->min_num;
        atime = param->p3->active / param->num3 / 1000;
        btime = param->p3->block / param->num3 / 1000;
        pthread_mutex_unlock(&param->p3->thcount_lock);
        sprintf(logbuffer, "线程池中线程平均活跃时间:%lf\t阻塞时间:%lf\n线程最高活跃数量:%d\t最低活跃数量:%d\t平均活跃数量:%d\n消息队列中消息的长度:%d\n\n", atime, btime, max_num, min_num, avg_mun, len);
        if ((fd = open("monitor.log", O_CREAT | O_WRONLY | O_APPEND, 0644)) >= 0)
        {
            (void)write(fd, logbuffer, strlen(logbuffer));
            (void)write(fd, "\n", 1);
            (void)close(fd);
        }
    }
}

int main(int argc, char** argv)
{
    int i, port, pid, listenfd, socketfd, hit;
    socklen_t length;
    static struct sockaddr_in cli_addr;  /* static = initialised to zeros */
    static struct sockaddr_in serv_addr; /* static = initialised to zeros */
    if (argc < 3 || argc > 3 || !strcmp(argv[1], "-?"))
    {
        (void)printf("hint: nweb Port-Number Top-Directory\t\tVERSION %d\n\n"
            "\tnweb is a small and very safe mini web server\n"
            "\tnweb only servers out file/web pages with extensions named below\n"
            "\t and only from the named directory or its sub-directories.\n"
            "\tThere is no fancy features = safe and secure.\n\n"
            "\tExample: nweb 8181 /home/nwebdir &\n\n"
            "\tOnly Supports:",
            VERSION);
        for (i = 0; extensions[i].ext != 0; i++)
            (void)printf(" %s", extensions[i].ext);
        (void)printf("\n\tNot Supported: URLs including \"..\", Java, Javascript, CGI\n"
            "\tNot Supported: directories / /etc /bin /lib /tmp /usr /dev /sbin \n"
            "\tNo warranty given or implied\n\tNigel Griffiths nag@uk.ibm.com\n");
        exit(0);
    }
    if (!strncmp(argv[2], "/", 2) || !strncmp(argv[2], "/etc", 5) ||
        !strncmp(argv[2], "/bin", 5) || !strncmp(argv[2], "/lib", 5) ||
        !strncmp(argv[2], "/tmp", 5) || !strncmp(argv[2], "/usr", 5) ||
        !strncmp(argv[2], "/dev", 5) || !strncmp(argv[2], "/sbin", 6))
    {
        (void)printf("ERROR: Bad top directory %s, see nweb -?\n", argv[2]);
        exit(3);
    }
    if (chdir(argv[2]) == -1)
    {
        (void)printf("ERROR: Can't Change to directory %s\n", argv[2]);
        exit(4);
    }
    (void)signal(SIGCLD, SIG_IGN); /* ignore child death */
    (void)signal(SIGHUP, SIG_IGN); /* ignore terminal hangups */
    for (i = 0; i < 32; i++)
        (void)close(i); /* close open files */
    (void)setpgrp();    /* break away from process group */
    logger(LOG, "tpweb starting", argv[1], getpid());
    /* setup the network socket */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        logger(ERROR, "system call", "socket", 0);
    port = atoi(argv[1]);
    if (port < 0 || port > 60000)
        logger(ERROR, "Invalid port number (try 1->60000)", argv[1], 0);

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
        logger(ERROR, "system call", "bind", 0);
    if (listen(listenfd, 64) < 0)
        logger(ERROR, "system call", "listen", 0);

    threadpool* rm = initTheadPool(60);
    threadpool* rf = initTheadPool(150);
    threadpool* sm = initTheadPool(2000);

    monitorFP* p = (monitorFP*)malloc(sizeof(monitorFP));
    p->p1 = rm;
    p->p2 = rf;
    p->p3 = sm;
    p->num1 = 80;
    p->num2 = 160;
    p->num3 = 40;
    pthread_t pth;
    pthread_create(&pth, NULL, monitor, p);

    for (hit = 1;; hit++)
    {
        length = sizeof(cli_addr);
        if ((socketfd = accept(listenfd, (struct sockaddr*)&cli_addr, &length)) < 0)
            logger(ERROR, "system call", "accept", 0);
        webparam* param = malloc(sizeof(webparam));
        param->hit = hit;
        param->fd = socketfd;
        param->rftp = rf;
        param->smtp = sm;
        param->buffer = (char*)malloc(sizeof(char) * (BUFSIZE + 1));
        task* t = (task*)malloc(sizeof(task));
        t->arg = param;
        t->function = readMsg;
        addTask2ThreadPool(rm, t);
    }

}
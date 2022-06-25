#ifndef H_SERVER
#define H_SERVER

#include <arpa/inet.h>
#include <cassert>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../http_conn/http_conn.h"
#include "../threadpool/threadpool.h"

const int MAX_FD = 65536; //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5; //最小超时单位

class Server {
public:
    Server();
    ~Server();

    void init(int port, string user, string passWord, string dbName,
    int log_write, int sql_num, int close_log, int actormodel, int thread_num);

    void log_write();
    void sql_pool();
    void thread_pool();
    void eventListen();
    void eventLoop();

    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);
    bool dealclinetdata();
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);


    int m_port;
    char* m_root;
    int m_log_write;
    int m_close_log;
    int m_actormodel;

    int m_pipefd[2];
    int m_epollfd;
    http_conn *users;

    //数据库相关
    connection_pool *m_connPool;
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    int m_sql_num;

    //线程池相关
    threadpool<http_conn> *m_pool;

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];
    int m_thread_num;

    int m_listenfd;
    int m_OPT_LINGER; //优雅关闭链接

    //定时器相关
    client_data *users_timer;
    Utils utils;
};

#endif
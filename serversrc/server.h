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

const int MAX_FD = 65536; //����ļ�������
const int MAX_EVENT_NUMBER = 10000; //����¼���
const int TIMESLOT = 5; //��С��ʱ��λ

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

    //���ݿ����
    connection_pool *m_connPool;
    string m_user;         //��½���ݿ��û���
    string m_passWord;     //��½���ݿ�����
    string m_databaseName; //ʹ�����ݿ���
    int m_sql_num;

    //�̳߳����
    threadpool<http_conn> *m_pool;

    //epoll_event���
    epoll_event events[MAX_EVENT_NUMBER];
    int m_thread_num;

    int m_listenfd;
    int m_OPT_LINGER; //���Źر�����

    //��ʱ�����
    client_data *users_timer;
    Utils utils;
};

#endif
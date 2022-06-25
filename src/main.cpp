#include <errno.h>
#include <sys/epoll.h>

#include "../tool/read_conf.cpp"
//#include "../tool/my_daemon.cpp"
#include "../tool/net_tools.cpp"

#define DEBUG_PRINT 1
#define PORT 0
#define IPADDRESS 1
#define DAEMON 2

const char* conffile = "./disk.conf";
const char* logfile = "./disk.log";
const int conf_num = 3;
const char* conf_item[][2] = {
    { "port", "80" }, { "ipaddress", "0.0.0.0" }, { "daemon", "0" }, { NULL, NULL }
};
char conf_value[conf_num][ITEM_LENGTH];
char log[2048];

char finalbuf[1500];
void printlog(char* buf)
{
    FILE* fplog = fopen(logfile, "a");
    if (fplog == NULL) {
        printf("%s Open Log Error.\n", logfile);
        exit(0);
    }
    char timebuf[21];
    time_t ltime;
    struct tm* today;
    time(&ltime);
    today = localtime(&ltime);

    memset(timebuf, 0, sizeof(timebuf));
    strftime(timebuf, 21, "%Y/%m/%d %T%n", today);
    timebuf[strlen(timebuf) - 1] = '\0';
    sprintf(finalbuf, "[%s]:%s", timebuf, buf);
    fputs(finalbuf, fplog);
    if (DEBUG_PRINT)
        printf("%s\n", buf);
    fclose(fplog);
}

int main()
{
    readConf(conffile, conf_item, conf_value, conf_num);
    const int server_port = atoi(conf_value[PORT]);
    if (server_port > 65535 || server_port < 0) {
        printlog("服务器端口超出范围[1..65535]，结束进程\n");
        return 0;
    }
    if (atoi(conf_value[DAEMON]))
        daemon(1, 1);
    int host_sock = startupserver_opt(conf_value[IPADDRESS], conf_value[PORT],
        REUSEADDR | NONBLOCK, 0, 0);
    if (host_sock < 0) {
        printlog("startupserver_opt error.\n");
        printlog(strerror(errno));
        return 0;
    }
    int epoll_fd = epoll_create(256);
    if (epoll_fd < 0) {
        printlog("epoll_create error.\n");
        printlog(strerror(errno));
        return 0;
    }
    struct epoll_event ev;
    struct epoll_event ret_ev[64];
    int ret_num = 64;
    int read_num = -1;
    ev.events = EPOLLIN;
    ev.data.fd = host_sock;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, host_sock, &ev) < 0) {
        printlog("host_sock epoll_ctl error.\n");
        printlog(strerror(errno));
        return 0;
    }
    int timeout = -1;
    int clilen;
    struct sockaddr_in cliaddr;
    while (1) {
        if ((read_num = epoll_wait(epoll_fd, ret_ev, ret_num, timeout)) < 0) {
            printlog("epoll_wait error.\n");
            printlog(strerror(errno));
            break;
        }
        for (int i = 0; i < read_num; ++i) {
            if (ret_ev[i].data.fd == host_sock && (ret_ev[i].events & EPOLLIN)) {
                int fd = ret_ev[i].data.fd;
                int newsock = accept(fd, (struct sockaddr*)&cliaddr, &clilen);
                if (newsock < 0) {
                    printlog("accept newsock error.\n");
                    printlog(strerror(errno));
                    continue;
                }
                setoptval(newsock, REUSEADDR | NONBLOCK, 0, 0);
                ev.events = EPOLLIN | EPOLLOUT;
                ev.data.fd = newsock;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, host_sock, &ev) < 0) {
                    sprintf(log, "newsock [%s: %d with %d] epoll_ctl error.\n",
                        inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port),
                        newsock);
                    printlog(log);
                    printlog(strerror(errno));
                    continue;
                }
                sprintf(log, "get a new client[%s: %d with %d]\n",
                    inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port), newsock);
                printlog(log);
            } else {
                if (ret_ev[i].events & EPOLLIN) {
                    int fd = ret_ev[i].data.fd;
                    char* buf = (char*)malloc(1030 * sizeof(char));
                    if (!buf) {
                        printlog(strerror(errno));
                        continue;
                    }
                    ssize_t _s = recv(fd, buf, 1024 * sizeof(char), 0);
                } else if (ret_ev[i].events & EPOLLOUT) {
                } else {
                }
            }
        }
    }
    return 0;
}
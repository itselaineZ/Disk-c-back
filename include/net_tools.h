#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <netinet/in.h>

#define REUSEADDR 1
#define RCVLOWAT 2
#define GETRBUF 4
#define GETWBUF 8
#define NONBLOCK 16

int startupserver(char *ip, int port);
int startupclient(int port);

int startupserver_opt(char *ip, int port, int opt, int setrbuf, int setwbuf);

int startupclient_opt(char *ip, int port, int opt, int setrbuf, int setwbuf);

int get_local_ip(char (*ip)[20]);

int setoptval(int sock, int opt, int setrbuf, int setwbuf);
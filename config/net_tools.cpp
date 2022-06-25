#include "../include/net_tools.h"

int startupserver(char *ip, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("sock");
        return -1;
    }
    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = inet_addr(ip);

    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    socklen_t len = sizeof(local);
    if (bind(sock, (struct sockaddr *)&local, len) < 0)
    {
        perror("bind");
        return -1;
    }
    if (listen(sock, 3) < 0)
    {
        perror("listen");
        return -1;
    }
    return sock;
}

int startupclient(int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("sock");
        return -1;
    }

    int optval = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in client;
    memset(&client, 0, sizeof(client));
    client.sin_family = AF_INET;
    if (port != -1)
        client.sin_port = htons(port);

    socklen_t len = sizeof(client);
    if (bind(sock, (struct sockaddr *)&client, len) < 0)
    {
        perror("bind");
        return -1;
    }
    return sock;
}

int startupserver_opt(char *ip, int port, int opt, int setrbuf, int setwbuf)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("sock");
        return -1;
    }
    struct sockaddr_in local;
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = inet_addr(ip);

    setoptval(sock, (opt&(!NONBLOCK)), setrbuf, setwbuf);

    socklen_t len = sizeof(local);
    if (bind(sock, (struct sockaddr *)&local, len) < 0)
    {
        perror("bind");
        return -1;
    }
    if (listen(sock, 3) < 0)
    {
        perror("listen");
        return -1;
    }
    if (opt&NONBLOCK)
        setoptval(sock, NONBLOCK, setrbuf, setwbuf);
    return sock;
}

int startupclient_opt(char *ip, int port, int opt, int setrbuf, int setwbuf)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("sock");
        return -1;
    }

    setoptval(sock, opt, setrbuf, setwbuf);

    struct sockaddr_in client;
    memset(&client, 0, sizeof(client));
    client.sin_family = AF_INET;
    if (port > 0)
        client.sin_port = htons(port);

    socklen_t len = sizeof(client);
    if (bind(sock, (struct sockaddr *)&client, len) < 0)
    {
        perror("bind");
        return -1;
    }
    return sock;
}

int get_local_ip(char (*ip)[20])
{
    int fd, intrface, retn = 0;
    char *ipp;
    struct ifreq buf[INET_ADDRSTRLEN];
    struct ifconf ifc;
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) >= 0)
    {
        ifc.ifc_len = sizeof(buf);
        ifc.ifc_buf = (caddr_t)buf;
        if (!ioctl(fd, SIOCGIFCONF, (char *)&ifc))
        {
            intrface = ifc.ifc_len / sizeof(struct ifreq);
            while (intrface-- > 0)
            {
                if (!(ioctl(fd, SIOCGIFADDR, (char *)&buf[intrface])))
                {
                    ipp = (inet_ntoa(((struct sockaddr_in *)(&buf[intrface].ifr_addr))->sin_addr));
                    strcpy(ip[retn++], ipp);
                }
            }
        }
        close(fd);
    }
    return retn;
}

int setoptval(int sock, int opt, int setrbuf, int setwbuf)
{
    int optval = 1, status;
    socklen_t optlen;
    optlen = sizeof(optval);
    if (opt & REUSEADDR)
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    if (opt & RCVLOWAT)
        setsockopt(sock, SOL_SOCKET, SO_RCVLOWAT, &optval, sizeof(optval));
    if (opt & GETRBUF)
    {
        getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &optval, &optlen);
        printf("原始接收缓冲区大小为 %d\n", optval);
    }
    if (opt & GETWBUF)
    {
        getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &optval, &optlen);
        printf("原始发送缓冲区大小为 %d\n", optval);
    }
    if (setrbuf > 0)
    {
        setrbuf *= 1024;
        status = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char *)&setrbuf, sizeof(setrbuf));
        if (status < 0)
        {
            perror("设置接收缓冲区错误\n");
            return -1;
        }
        getsockopt(sock, SOL_SOCKET, SO_RCVBUF, &optval, &optlen);
        printf("当前接收缓冲区大小为 %d\n", optval);
    }
    if (setwbuf > 0)
    {
        setwbuf *= 1024;
        status = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&setwbuf, sizeof(setwbuf));
        if (status < 0)
        {
            perror("设置发送缓冲区错误\n");
            return -1;
        }
        getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &optval, &optlen);
        printf("当前发送缓冲区大小为 %d\n", optval);
    }
    if (opt & NONBLOCK)
    {
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
}
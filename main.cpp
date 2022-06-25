#include "./config/config.h"
#include "./serversrc/server.h"

int main()
{
    Config config("./disk.conf");
    Server server;
    server.init(config.port, config.user, config.passwd, config.databasename,
        config.LOGWrite, config.sql_num, config.close_log, config.actormodel, config.thread_num);
    server.log_write();
    server.sql_pool();
    printf("cccc\n");
    server.thread_pool();
    printf("dddd\n");
    server.eventListen();
    printf("eeee\n");
    server.eventLoop();
    return 0;
}
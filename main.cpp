#include "./config/config.h"
#include "./serversrc/server.h"

int main()
{
    Config config("./disk.conf");
    Server server;
    server.init(config.port, config.user, config.passwd, config.databasename,
        config.LOGWrite, config.sql_num, config.close_log, config.actormodel, 
        config.thread_num, config.file_thread_num);
    server.log_write();
    server.sql_pool();
    server.thread_pool();
    //server.file_pool();
    server.task_pool();
    server.eventListen();
    server.eventLoop();
    return 0;
}
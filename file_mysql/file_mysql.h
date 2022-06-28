#ifndef H_FILE_MYSQL
#define H_FILE_MYSQL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "../CGImariadb/sql_connection_pool.h"

class file_mysql {
public:
    file_mysql() { }
    ~file_mysql() { }
    bool read_once();
    bool write();
    void process();

public:
    MYSQL* mysql;
    //为了复用线程池增加的与http_conn相同的变量
    int m_state;
    int improv;
    int timer_flag;

private:
    char* buf;

};

#endif
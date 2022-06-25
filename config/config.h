#ifndef H_READ_CONF
#define H_READ_CONF
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
using namespace std;

const int ITEM_LENGTH = 50;

#define ANALYSIS_FAILURE 0;
#define ANALYSIS_SUCCESS 1;

class Config {
public:
    Config();
    Config(const char* conffile);
    ~Config() {};

    int port, LOGWrite, sql_num, close_log, actormodel, thread_num;
    string user;
    string passwd;
    string databasename;
};
#endif
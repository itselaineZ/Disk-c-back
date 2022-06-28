#include "config.h"

const int conf_num = 10;
const char* conf_item[][2] = {
    { "port", "80" }, 
    { "LOGWrite", "0" },
    { "sql_num", "8" },
    { "close_log", "0" },
    { "user", "root" },
    { "passwd", "ZXYgyt216216@" },
    { "databasename", "disk" },
    { "actormodel", "0" },
    { "threadnum", "8" },
    { "filethreadnum", "4" },
    { NULL, NULL }
};
char conf_value[conf_num][ITEM_LENGTH];

int strkv(char* src, char* key, char* value)
{
    char *p, *q;
    int len, i;
    p = strchr(src, '=');
    q = strchr(src, '\n');
    if (p && q) {
        *q = '\0';
        for (i = 1; p - i > src && (*(p - i) == ' ' || *(p - i) == '\t'); ++i)
            ;
        if (p - i == src)
            return 0;
        strncpy(key, src, p - src - i + 1);
        key[p - src - i + 1] = 0;
        for (i = 1; p + i < q && (*(p + i) == ' ' || *(p + i) == '\t'); ++i)
            ;
        if (p + i == q)
            return 0;
        strcpy(value, p + i);
        return 1;
    }
    return 0;
}

// conf_ad配置文件地址，item配置参数列表，参数数量
int readConf(const char* conf_ad, const char* item[][2], char itemvalue[][ITEM_LENGTH], const int num)
{
    FILE* pfile = fopen(conf_ad, "r");
    if (pfile == NULL) {
        printf("打开配置文件 %s 失败\n", conf_ad);
        return ANALYSIS_FAILURE;
    }
    char buf[50] = "";
    char key[50] = "";
    char value[50] = "";
    while (fgets(buf, 50, pfile)) {
        if (strkv(buf, key, value)) {
            for (int i = 0; i < num; ++i)
                if (strcmp(key, item[i][0]) == 0)
                    strcpy(itemvalue[i], value);
            memset(key, 0, sizeof(key));
        }
    }
    fclose(pfile);
    for (int i = 0; i < num; ++i)
        if (strcmp(itemvalue[i], "") == 0) {
            if (item[i][1] == NULL) {
                printf("配置参数不足\n");
                return ANALYSIS_FAILURE;
            } else
                strcpy(itemvalue[i], item[i][1]);
        }
    return ANALYSIS_SUCCESS;
}

Config::Config() { }

Config::Config(const char* conffile)
{
    readConf(conffile, conf_item, conf_value, conf_num);
    this->port = atoi(conf_value[0]);
    this->LOGWrite = atoi(conf_value[1]);
    this->sql_num = atoi(conf_value[2]);
    this->close_log = atoi(conf_value[3]);
    this->user = string(conf_value[4]);
    this->passwd = string(conf_value[5]);
    this->databasename = string(conf_value[6]);
    this->actormodel = atoi(conf_value[7]);
    this->thread_num = atoi(conf_value[8]);
    this->file_thread_num = atoi(conf_value[9]);
}
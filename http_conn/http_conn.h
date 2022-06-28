#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <map>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../CGImariadb/sql_connection_pool.h"
#include "../lock/locker.h"
#include "../log/log.h"
#include "../taskpool/taskpool.h"
#include "../timer/timer.h"

class http_conn {
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    static const int AUTHORIZATION_LEN = 100;
    enum METHOD {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    enum LINE_STATUS {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };
    enum RESPONSE_DATA_TYPE {
        RES_MESSAGE = 0,
        RES_FILE
    };
    enum MYSQL_OP{
        INSERT_MYSQL = 0,
        UPDATE_MYSQL,
        DELETE_MYSQL
    };

public:
    http_conn() { }
    ~http_conn() { }

public:
    void init(int sockfd, const sockaddr_in& addr, char*, int, string user, string passwd, string sqlname, TASKPOOL* tskpool);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    void gen_auth();
    void clear_auth();
    sockaddr_in* get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool* connPool);
    void init_taskpool(TASKPOOL* tskpool);
    int timer_flag;
    int improv;

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();
    HTTP_CODE login_request(const char* name, const char* password);
    HTTP_CODE register_request(const char* name, const char* password);
    HTTP_CODE getdir_request(const char* name, const char* pth);
    HTTP_CODE mkdir_request();
    HTTP_CODE getstate_request(const char* name);
    HTTP_CODE changename_request(const char* name, const char* oldpath, const char* newpath);
    HTTP_CODE movefile_request(const char* name, const char* oldpath, const char* newpath);
    HTTP_CODE copyfile_request(const char* name, const char* oldpath, const char* newpath);
    HTTP_CODE delfile_request(const char* name, const char* pth);
    HTTP_CODE uploadsingle_request();
    HTTP_CODE insertfile_mysql(bool flg);
    HTTP_CODE userfile_mysql();
    HTTP_CODE downloadsingle_request();
    HTTP_CODE breakpoint_mysql(int flg);
    HTTP_CODE getall_request(const char* name);
    char* get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    bool file_exist();
    bool save_file();
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_headers(int content_length, char* auth);
    bool add_authorization(char* auth);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL* mysql;
    TASKPOOL* m_tp;
    int m_state; //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx;
    int m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char* m_url;
    char* m_version;
    char* m_host;
    int m_content_length;
    char* m_content_type;
    char* m_upload_file; // 有值表示处于上传状态
    int m_slice_id; // 上传上来的当前这片数据所属id
    char* m_username; // 只在上传下载的时候有效
    char* m_savepath; // 只在上传下载的时候有效
    char* m_file_type;
    int m_file_size;
    bool m_linger; // Keep-alive
    char* m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi; //是否启用的POST
    char* m_string; //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    char* doc_root;
    string m_res;
    char m_auth[AUTHORIZATION_LEN];
    bool m_with_auth;
    bool m_res_type;
    int m_slice_assign; // 分配下去需要发回给服务器的数据id

    map<string, string> m_users;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
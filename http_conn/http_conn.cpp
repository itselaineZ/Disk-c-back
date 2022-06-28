#include "http_conn.h"
#include "../MD5/md5.h"

#include <fstream>
#include <mysql/mysql.h>

//定义http响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file form this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;
map<int, string> authlist;

void http_conn::initmysql_result(connection_pool* connPool)
{
    //先从连接池中取一个连接
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT name,password FROM users")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES* result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

void http_conn::init_taskpool(TASKPOOL* tskpool)
{
    m_tp = tskpool;
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

    event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT，保证同一SOCKET只能被一个线程处理，不会跨越多个线程。
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//将密码加密为MD5形式
string ToKey(string Password)
{
    MD5 md5; //定义MD5的类
    md5.update(Password);
    string strKey = md5.toString(); //转换为key秘钥
    md5.reset(); //重置MD5
    string cstrKey(strKey.c_str()); // key格式转换为CString
    return cstrKey; //返回的是MD5加密的密码
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1)) {
        LOG_INFO("clear client(%d)", m_sockfd);
        //clear_auth();
        if (m_upload_file) { // 是一个上传状态的client，离开时需要从队列中移除
            m_tp->RemoveSock(m_upload_file, m_sockfd);
            m_tp->RecoverSlice(m_upload_file, m_slice_assign); // 连接到底会关闭吗？？？
        }
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//生成auth
void http_conn::gen_auth()
{
    strcpy(m_auth, std::to_string(m_sockfd).c_str());
    authlist.insert(pair<int, string>(m_sockfd, m_auth));
}
void http_conn::clear_auth()
{
    if (authlist.find(m_sockfd) != authlist.end())
        authlist.erase(m_sockfd);
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in& addr, char* root,
    int close_log, string user, string passwd, string sqlname, TASKPOOL* tskpool)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    m_tp = tskpool;

    init();
}

//初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;
    m_res = "";
    m_with_auth = true;
    m_res_type = RES_MESSAGE;
    m_content_type = 0;
    m_upload_file = 0;
    m_slice_id = -1;
    m_username = 0;
    m_savepath = 0;
    m_file_type = 0;
    m_file_size = -1;
    m_slice_assign = -1;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (m_check_state != CHECK_STATE_CONTENT) {
            if (temp == '\r') {
                if ((m_checked_idx + 1) == m_read_idx)
                    return LINE_OPEN;
                else if (m_read_buf[m_checked_idx + 1] == '\n') { //把\r\n换成00
                    m_read_buf[m_checked_idx++] = '\0';
                    m_read_buf[m_checked_idx++] = '\0';
                    return LINE_OK;
                }
                return LINE_BAD;
            } else if (temp == '\n') {
                if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') { //把\r\n换成00
                    m_read_buf[m_checked_idx - 1] = '\0';
                    m_read_buf[m_checked_idx++] = '\0';
                    return LINE_OK;
                }
                return LINE_BAD;
            }
        } else { // content里不需要解析
            if (temp != '\0') {
                return LINE_OK;
            }
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    int bytes_read = 0;

    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;

    if (bytes_read <= 0) {
        return false;
    }

    return true;
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    m_url = strpbrk(text, " \t"); //找第一次出现spc或tab的位置
    if (!m_url) {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char* method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;
    } else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t"); //包含spc、tab的部分的长度
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    if (text[0] == '\0') {
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else if (strncasecmp(text, "Authorization:", 14) == 0) {
        text += 14;
        int p = strspn(text, " \t");
        strncpy(m_auth, text, p);
        m_auth[p] = '\0';
        text += p;

    } else if (strncasecmp(text, "Content-Type:", 13) == 0) {
        text += 13;
        text += strspn(text, " \t");
        m_content_type = text;
        int p = strspn(text, "; \t");
        text += p;
        *text = '\0';
    } else if (strncasecmp(text, "Upload_File:", 12) == 0) {
        text += 12;
        text += strspn(text, " \t");
        m_upload_file = text;
        text = strpbrk(text, "; ");
        *text++ = '\0';
        text = strstr(text, "slice=");
        if (text) {
            text += 6;
            m_slice_id = atoi(text);
        } else
            m_slice_id = -1;
        // m_linger = 1;
    } else if (strncasecmp(text, "User_name:", 10) == 0) {
        text += 10;
        text += strspn(text, " \t");
        m_username = text;
    } else if (strncasecmp(text, "Save_path:", 10) == 0) {
        text += 10;
        text += strspn(text, " \t");
        m_savepath = text;
    } else if (strncasecmp(text, "File_type:", 10) == 0) {
        text += 10;
        text += strspn(text, " \t");
        m_file_type = text;
    } else if (strncasecmp(text, "File_size:", 10) == 0) {
        text += 10;
        text += strspn(text, " \t");
        m_file_size = atoi(text);
    } else {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    // printf("m_read_idx:%d,m_content_length:%d,m_checked_idx:%d\n", m_read_idx, m_content_length, m_checked_idx);
    // printf("text:%s\n", text);
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OPEN) || ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();
        switch (m_check_state) {
        case CHECK_STATE_REQUESTLINE: {
            m_start_line = m_checked_idx; //下一次读的开始地址移动到当前行检查的位置
            LOG_INFO("%s", text);
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER: {
            m_start_line = m_checked_idx; //下一次读的开始地址移动到当前行检查的位置
            LOG_INFO("%s", text);
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST) {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT: {
            ret = parse_content(text);
            if (ret == GET_REQUEST) {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // printf("m_url:%s\n", m_url);
    const char* p = strrchr(m_url, '/');
    p += 3;
    printf("p:%s, cgi:%d\n", p, cgi);

    //处理cgi
    if (cgi == 1) {
        if (*(p + 1) == '0' || *(p + 1) == '1') {
            //将用户名和密码提取出来
            // username=123&password=123
            char name[100], password[100];
            int i;
            for (i = 9; m_string[i] != '&'; ++i)
                name[i - 9] = m_string[i];
            name[i - 9] = '\0';

            int j = 0;
            for (i = i + 10; m_string[i] != '\0' && m_string[i] != '\t' && m_string[i] != ' '; ++i, ++j)
                password[j] = m_string[i];
            password[j] = '\0';

            if (!name[0] || !password[0]) {
                LOG_ERROR("GET Name/password failed");
                return INTERNAL_ERROR;
            }
            if (m_with_auth)
                gen_auth();
            if (*(p + 1) == '1') { //注册
                m_res_type = RES_MESSAGE;
                return register_request(name, password);
            } else { // 登录
                m_res_type = RES_MESSAGE;
                return login_request(name, password);
            }
        } else if (*(p + 1) == '2' || *(p + 1) == '8') {
            // path=xx&username=xx
            char* pth = strstr(m_string, "path=");
            pth += 5;
            char* name = pth;
            name = strpbrk(pth, " \t,&");
            *name++ = '\0';
            name += 9;
            char* pa = strpbrk(name, " \t,\r\n");
            if (pa)
                *(pa) = '\0';
            LOG_INFO("query: pth=%s, username=%s\n", pth, name);
            if (*(p + 1) == '2') { // 查询文件
                m_res_type = RES_FILE;
                HTTP_CODE rt = getdir_request(name, pth);
                if (rt == INTERNAL_ERROR)
                    return INTERNAL_ERROR;
            } else { // 删除文件/文件夹
                m_res_type = RES_MESSAGE;
                return delfile_request(name, pth);
            }
        } else if (*(p + 1) == '3') { // 创建目录
            m_res_type = RES_MESSAGE;
            return mkdir_request();
        } else if (*(p + 1) == '4' || *(p + 1) == 'd') { // 查询文件未完成状态
            // username=xx
            char* name = strstr(m_string, "username=");
            name += 9;
            char* pa = strpbrk(name, " \t\n\r");
            if (pa)
                *pa = '\0';
            printf("name:%s\n", name);
            if (*(p + 1) == '4') {
                m_res_type = RES_FILE;
                HTTP_CODE rt = getstate_request(name);
                if (rt == INTERNAL_ERROR)
                    return INTERNAL_ERROR;
            } else {
                m_res_type = RES_FILE;
                HTTP_CODE rt = getall_request(name);
                if (rt == INTERNAL_ERROR)
                    return INTERNAL_ERROR;
            }
        } else if (*(p + 1) == '5' || *(p + 1) == '6' || *(p + 1) == '7') {
            // username=xx&oldpath=xx&newpath=xx
            char* name = strstr(m_string, "username=");
            name += 9;
            char* oldpath = name;
            oldpath = strpbrk(oldpath, " &,\n\r");
            *oldpath++ = '\0';
            oldpath += 8;
            char* newpath = oldpath;
            newpath = strpbrk(newpath, " &,\n\r");
            *newpath++ = '\0';
            newpath += 8;
            char* pa = strpbrk(newpath, " \t\r\n");
            if (pa)
                *pa = '\0';
            m_res_type = RES_MESSAGE;

            char* sql_insert = (char*)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "SELECT f_path FROM user_file WHERE u_name='");
            strcat(sql_insert, name);
            strcat(sql_insert, "' AND f_type='");
            strcat(sql_insert, newpath);
            strcat(sql_insert, "'");
            LOG_INFO(sql_insert);
            if (mysql_query(mysql, sql_insert)) {
                LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
                free(sql_insert);
                return INTERNAL_ERROR;
            }
            MYSQL_RES* result = mysql_store_result(mysql);
            int num_fields = mysql_num_fields(result);
            MYSQL_FIELD* fields = mysql_fetch_fields(result);
            if (MYSQL_ROW row = mysql_fetch_row(result)) {
                m_res = "-2";
                LOG_INFO("newfilepath ALREADY EXISTS: newpath=%s\n", newpath);
                free(sql_insert);
                return FILE_REQUEST;
            }
            if (*(p + 1) == '5') { // 单文件/单文件夹改名
                return changename_request(name, oldpath, newpath);
            } else if (*(p + 1) == '6') { // 文件/文件夹移动
                return movefile_request(name, oldpath, newpath);
            } else { // 文件/文件夹复制
                return copyfile_request(name, oldpath, newpath);
            }
        } else if (*(p + 1) == '9') { // 单个文件上传
            // username=xx&filename=xx&filetype=xx&filesize=xx&filepath=xx
            m_res_type = RES_MESSAGE;
            return uploadsingle_request();
        } else if (*(p + 1) == 'a') { // 单个文件下载

        } else if (*(p + 1) == 'b') { // 文件夹上传

        } else if (*(p + 1) == 'c') { // 文件夹下载

        } else {
            return BAD_REQUEST;
        }
    }

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH)) //其他组读权限
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode)) //目录
        return BAD_REQUEST;
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::register_request(const char* name, const char* password)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    //如果是注册，先检测数据库中是否有重名的
    //没有重名的，进行增加数据
    string md5key = ToKey(password);
    strcpy(sql_insert, "INSERT INTO users(name, password) VALUES(");
    strcat(sql_insert, "'");
    strcat(sql_insert, name);
    strcat(sql_insert, "', '");
    strcat(sql_insert, md5key.c_str());
    strcat(sql_insert, "')");

    if (users.find(name) == users.end()) {
        LOG_INFO(sql_insert);
        m_lock.lock();
        int res = mysql_query(mysql, sql_insert);
        if (!res)
            users.insert(pair<string, string>(name, md5key));
        m_lock.unlock();

        if (res) {
            m_res = "-1";
            LOG_ERROR("QUERY users ERROR, res=%d:%s\n", res, mysql_error(mysql));
            free(sql_insert);
            return INTERNAL_ERROR;
        } else {
            strcpy(sql_insert, "INSERT INTO user_file(u_name, f_path, change_time, f_type) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '/");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            time_t t = time(NULL);
            struct tm* sys_tm = localtime(&t);
            struct tm my_tm = *sys_tm;
            char nowtm[30];
            sprintf(nowtm, "%d-%02d-%02d %02d:%02d:%02d",
                my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec);
            strcat(sql_insert, nowtm);
            strcat(sql_insert, "','dir')");
            LOG_INFO(sql_insert);

            m_lock.lock();
            res = mysql_query(mysql, sql_insert);
            m_lock.unlock();

            if (res) {
                m_res = "-1";
                LOG_ERROR("QUERY user_file create rootdir ERROR, res = %d:%s\n", res, mysql_error(mysql));
                free(sql_insert);
                return INTERNAL_ERROR;
            } else
                m_res = "0";
        }
    } else {
        m_res = "1";
        LOG_INFO(" Register Name Repeated, client(%s)\n", inet_ntoa(m_address.sin_addr));
    }
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::login_request(const char* name, const char* password)
{
    //如果是登录，直接判断
    //若浏览器端输入的用户名和密码在表中可以查找到，返回0，否则返回-1
    string md5key = ToKey(password);
    if (users.find(name) != users.end() && users[name] == md5key)
        m_res = "0";
    else if (users.find(name) == users.end())
        m_res = "-1";
    else
        m_res = "-2";
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::getdir_request(const char* name, const char* pth)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);

    // if (authlist.find(m_sockfd) == authlist.end()) {
    //     LOG_ERROR("NO Auth ERROR, client(%s)\n", inet_ntoa(m_address.sin_addr));
    //     return INTERNAL_ERROR;
    // } else {
    // string auth = authlist[m_sockfd];
    // if (strcmp(m_auth, auth.c_str()) == 0) {
    // not '/username/[/]'
    strcpy(sql_insert, "SELECT f_path, change_time, f_type, f_size FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path REGEXP '");
    strcat(sql_insert, pth);
    strcat(sql_insert, ".*' AND f_path NOT REGEXP '");
    strcat(sql_insert, pth);
    strcat(sql_insert, ".*[/].*'");
    LOG_INFO(sql_insert);
    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    MYSQL_RES* result = mysql_store_result(mysql);
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);
    strcpy(m_real_file, name);
    strcat(m_real_file, ".filelst");
    LOG_INFO("create file:%s\n", m_real_file);

    ofstream rowfs(m_real_file, ios::out);
    rowfs << "[";
    while (MYSQL_ROW row = mysql_fetch_row(result)) {

        rowfs << "{\"f_path\":\"" << row[0] << "\",";
        rowfs << "\"change_time\":\"" << row[1] << "\",";
        if (row[2])
            rowfs << "\"f_type\":\"" << row[2] << "\",";
        else
            rowfs << "\"f_type\":\"null\",";
        if (row[3])
            rowfs << "\"f_size\":\"" << row[3] << "\"},\n";
        else
            rowfs << "\"f_size\":\"null\"},\n";
    }
    rowfs << "]";
    rowfs.close();
    // } else {
    //     LOG_INFO("Auth Failed, client(%s)\n", inet_ntoa(m_address.sin_addr));
    //     return BAD_REQUEST;
    // }
    //}
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::mkdir_request()
{
    // path=xx&username=xx
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    char* pth = strstr(m_string, "path=");
    pth += 5;
    char* name = pth;
    name = strpbrk(pth, " \t,&");
    *name++ = '\0';
    name += 9;
    char* p = strpbrk(name, " \t,\r\n");
    if (p)
        *(p) = '\0';
    LOG_INFO("create: pth=%s, username=%s\n", pth, name);
    strcpy(sql_insert, "INSERT INTO user_file(u_name, f_path, change_time, f_type) VALUES('");
    strcat(sql_insert, name);
    strcat(sql_insert, "', '");
    strcat(sql_insert, pth);
    strcat(sql_insert, "', '");
    time_t t = time(NULL);
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char nowtm[30];
    sprintf(nowtm, "%d-%02d-%02d %02d:%02d:%02d",
        my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
        my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec);
    strcat(sql_insert, nowtm);
    strcat(sql_insert, "','dir')");
    LOG_INFO(sql_insert);

    m_lock.lock();
    int res = mysql_query(mysql, sql_insert);
    m_lock.unlock();

    if (res) {
        m_res = "-1";
        LOG_ERROR("QUERY user_file create dir ERROR, res = %d:%s\n", res, mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    } else
        m_res = "0";
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::getstate_request(const char* name)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "SELECT f_path, f_size, f_type, breakpoint, op FROM file_breakpoint WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "'");
    LOG_INFO(sql_insert);

    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    MYSQL_RES* result = mysql_store_result(mysql);
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);
    strcpy(m_real_file, name);
    strcat(m_real_file, ".filelst");
    LOG_INFO("create file:%s\n", m_real_file);

    ofstream rowfs(m_real_file, ios::out);
    rowfs << "[";
    while (MYSQL_ROW row = mysql_fetch_row(result)) {

        rowfs << "{'f_path':'" << row[0] << "',";
        rowfs << "'f_size':'" << row[1] << "',";
        rowfs << "'f_type':'" << row[2] << "',";
        rowfs << "'breakpoint':'" << row[3] << "'},\n";
        rowfs << "'op':'" << row[4] << "'},\n";
    }
    rowfs << "]";
    rowfs.close();
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::changename_request(const char* name, const char* oldpath, const char* newpath)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "UPDATE user_file SET f_path='");
    strcat(sql_insert, newpath);
    strcat(sql_insert, "' WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path='");
    strcat(sql_insert, oldpath);
    strcat(sql_insert, "'");
    LOG_INFO(sql_insert);

    m_lock.lock();
    int res = mysql_query(mysql, sql_insert);
    m_lock.unlock();

    if (res) {
        m_res = "-1";
        free(sql_insert);
        LOG_ERROR("UPDATE user_file f_path ERROR, res = %d:%s\n", res, mysql_error(mysql));
        return INTERNAL_ERROR;
    } else
        m_res = "0";
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::copyfile_request(const char* name, const char* oldpath, const char* newpath)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    int len = strlen(oldpath);
    strcpy(sql_insert, "SELECT u_name,f_name,f_type,f_size,f_path,change_time FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND (f_path REGEXP '");
    strcat(sql_insert, oldpath);
    strcat(sql_insert, "/.*' OR f_path='");
    strcat(sql_insert, oldpath);
    strcat(sql_insert, "')");
    LOG_INFO(sql_insert);

    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }

    MYSQL_RES* result = mysql_store_result(mysql);
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);
    int res = 0;
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        strcpy(sql_insert, "INSERT INTO user_file(u_name,f_name,f_type,f_size,f_path,change_time) VALUES('");
        strcat(sql_insert, row[0]);
        if (row[1]) {
            strcat(sql_insert, "', '");
            strcat(sql_insert, row[1]);
            strcat(sql_insert, "'");
        } else {
            strcat(sql_insert, "', null");
        }
        strcat(sql_insert, ", '");
        strcat(sql_insert, row[2]);
        if (row[3]) {
            strcat(sql_insert, "', '");
            strcat(sql_insert, row[3]);
            strcat(sql_insert, "'");
        } else {
            strcat(sql_insert, "', null");
        }
        strcat(sql_insert, ", CONCAT('");
        strcat(sql_insert, newpath);
        strcat(sql_insert, "', SUBSTR(f_path, ");
        strcat(sql_insert, std::to_string(len + 1).c_str());
        strcat(sql_insert, ", CHAR_LENGTH(f_path)-");
        strcat(sql_insert, std::to_string(len + 1).c_str());
        strcat(sql_insert, ")), '");
        strcat(sql_insert, row[5]);
        strcat(sql_insert, "')");
        LOG_INFO(sql_insert);

        m_lock.lock();
        res = mysql_query(mysql, sql_insert);
        m_lock.unlock();

        if (res) {
            m_res = "-1";
            free(sql_insert);
            LOG_ERROR("INSERT user_file newpath ERROR, res = %d:%s", res, mysql_error(mysql));
            return INTERNAL_ERROR;
        }
    }
    m_res = "0";

    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::movefile_request(const char* name, const char* oldpath, const char* newpath)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    int len = strlen(oldpath);
    strcpy(sql_insert, "UPDATE user_file SET f_path=CONCAT('");
    strcat(sql_insert, newpath);
    strcat(sql_insert, "', SUBSTR(f_path, ");
    strcat(sql_insert, std::to_string(len + 1).c_str());
    strcat(sql_insert, ", CHAR_LENGTH(f_path)-");
    strcat(sql_insert, std::to_string(len + 1).c_str());
    strcat(sql_insert, ")) WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path='");
    strcat(sql_insert, oldpath);
    strcat(sql_insert, "'");
    LOG_INFO(sql_insert);

    m_lock.lock();
    int res = mysql_query(mysql, sql_insert);
    m_lock.unlock();

    if (res) {
        m_res = "-1";
        LOG_ERROR("UPDATE user_file newpath ERROR, res = %d:%s", res, mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    } else
        m_res = "0";
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::delfile_request(const char* name, const char* pth)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "SELECT f_name,f_type,f_size FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND (f_path REGEXP '");
    strcat(sql_insert, pth);
    strcat(sql_insert, "' OR f_path REGEXP '");
    strcat(sql_insert, pth);
    strcat(sql_insert, "/.*')");
    LOG_INFO(sql_insert);
    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    MYSQL_RES* result = mysql_store_result(mysql);
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    strcpy(sql_insert, "DELETE FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND (f_path REGEXP '");
    strcat(sql_insert, pth);
    strcat(sql_insert, "' OR f_path REGEXP '");
    strcat(sql_insert, pth);
    strcat(sql_insert, "/.*')");
    LOG_INFO(sql_insert);

    m_lock.lock();
    int res = mysql_query(mysql, sql_insert);
    m_lock.unlock();

    if (res) {
        m_res = "-1";
        LOG_ERROR("DELETE FROM user_file ERROR, res = %d:%s\n", res, mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }

    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        if (strcmp(row[1], "dir") != 0) {
            strcpy(sql_insert, "UPDATE files SET refer=refer-1 WHERE name='");
            strcat(sql_insert, row[0]);
            strcat(sql_insert, "' AND ftype='");
            strcat(sql_insert, row[1]);
            strcat(sql_insert, "' AND fsize=");
            strcat(sql_insert, row[2]);
            strcat(sql_insert, "'");
            LOG_INFO(sql_insert);

            m_lock.lock();
            res = mysql_query(mysql, sql_insert);
            m_lock.unlock();
            if (!res) {
                m_res = "-1";
                LOG_ERROR("UPDATE user_file refer ERROR, res = %d:%s\n", res, mysql_error(mysql));
                free(sql_insert);
                return INTERNAL_ERROR;
            }
        }
    }

    strcpy(sql_insert, "DELETE FROM files WHERE refer=0");
    LOG_INFO(sql_insert);
    m_lock.lock();
    res = mysql_query(mysql, sql_insert);
    m_lock.unlock();

    if (res) {
        m_res = "-1";
        LOG_ERROR("DELETE refer=0 FROM user_file ERROR, res = %d:%s\n", res, mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    } else
        m_res = "0";
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::uploadsingle_request()
{
    if (file_exist()) { // 是否数据库中已经有文件
        printf("file exists.\n");
        m_res = "{\"already\":1, \"all\":1}";
        HTTP_CODE rt = insertfile_mysql(false);
        if (rt == INTERNAL_ERROR)
            return INTERNAL_ERROR;
        return userfile_mysql();
    } else {
        if (m_string[0] == 'u') { // 首次提交上传申请
            printf("in upload\n");
            // upload
            LOG_INFO("upload REQUEST: username=%s,filemd5=%s,filetype=%s,filesize=%d,filepath=%s",
                m_username, m_upload_file, m_file_type, m_file_size, m_savepath);
            LOG_INFO("add task:%d\n", m_sockfd);
            m_tp->AddTask(m_upload_file, m_file_size, m_sockfd, m_file_type);
            // HTTP_CODE rt = breakpoint_mysql(true);
            // if (rt == INTERNAL_ERROR)
            //     return INTERNAL_ERROR;
        } else if (m_string[0] == 'w') { // 无分配数据传输情况下的保持联系，同步进度
            printf("in waiting\n");
            // int already = m_tp->GetRecvSize(m_upload_file);
            // int all = m_tp->GetAllSize(m_upload_file);
            // if (already == -1 || all == -1) {
            //     LOG_INFO("too late waiting: sockfd=%d", m_sockfd);
            //     return BAD_REQUEST;
            // }

            // m_res = "{\"already\":" + std::to_string(already) + ", \"all\":" + std::to_string(all) + "}";

            // if (already == all) {
            //     HTTP_CODE rt = insertfile_mysql(m_file_type, m_file_size, false);
            //     if (rt == INTERNAL_ERROR)
            //         return INTERNAL_ERROR;
            //     return userfile_mysql(m_file_type, m_file_size);
            // } else
            //     return FILE_REQUEST;
            // HTTP_CODE rt = breakpoint_mysql(false);
            // if (rt == INTERNAL_ERROR)
            //     return INTERNAL_ERROR;
        } else { // multipart/form-data传输数据
            LOG_INFO("to save:%d\n", m_sockfd);
            if (!m_tp->FindSock(m_sockfd, m_upload_file)) {
                LOG_INFO("too late upload: sockfd=%d", m_sockfd);
                return BAD_REQUEST;
            } else {
                int sv_rt = save_file();
                if (sv_rt == false)
                    return INTERNAL_ERROR;
                TASK_STATUS rc_rt;
                rc_rt = m_tp->RecvTask(m_upload_file, m_slice_id);
                if (rc_rt == REC_ERROR) {
                    LOG_ERROR("task dose not exists, task=%s", m_upload_file);
                    return BAD_REQUEST;
                } else if (rc_rt == MERGE_FAILED) {
                    LOG_ERROR("merge task[%s] failed", m_upload_file);
                    return INTERNAL_ERROR;
                } else if (rc_rt == MERGE_SUCCESS) {
                    m_res = "{\"already\":" + std::to_string(m_tp->GetRecvSize(m_upload_file))
                        + ", \"all\":" + std::to_string(m_tp->GetAllSize(m_upload_file))
                        + "}";
                    HTTP_CODE rt; // = breakpoint_mysql(false, );
                    rt = insertfile_mysql(true);
                    if (rt == INTERNAL_ERROR)
                        return INTERNAL_ERROR;
                    return userfile_mysql();
                }
            }
        }
        int offset_begin = m_tp->GetSlice(m_upload_file);
        if (offset_begin == -1) { // 任务丢失
            LOG_ERROR("task lost:%s", m_upload_file);
            return INTERNAL_ERROR;
        } else if (offset_begin < -1) { // 等待其他client上传完成，没有剩余slice可以用于分配
            LOG_INFO("client(%d:%s) waiting: all slice assigned", m_sockfd, m_username);
            m_res = "{\"already\":" + std::to_string(m_tp->GetRecvSize(m_upload_file))
                + ", \"all\":" + std::to_string(m_tp->GetAllSize(m_upload_file))
                + "}";
            return FILE_REQUEST;
        } else { // 成功分配到slice，要求上传
            int slicesize = m_tp->GetSliceSize();
            m_slice_assign = offset_begin / slicesize;
            m_res = "{\"slice\":" + std::to_string(m_slice_assign)
                + ", \"need_offset_begin\":" + std::to_string(offset_begin)
                + ", \"slice_size\":" + std::to_string(slicesize)
                + ", \"already\":" + std::to_string(m_tp->GetRecvSize(m_upload_file))
                + ", \"all\":" + std::to_string(m_tp->GetAllSize(m_upload_file))
                + "}";
            return FILE_REQUEST;
        }
    }
}

http_conn::HTTP_CODE http_conn::insertfile_mysql(bool flg)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    if (flg) {
        strcpy(sql_insert, "INSERT INTO files(name,ftype,fsize,content,refer) VALUES('");
        strcat(sql_insert, m_upload_file);
        strcat(sql_insert, "', '");
        strcat(sql_insert, m_file_type);
        strcat(sql_insert, "', ");
        strcat(sql_insert, std::to_string(m_file_size).c_str());
        strcat(sql_insert, "', '");
        strcat(sql_insert, m_upload_file);
        strcat(sql_insert, "', 1)");
        LOG_INFO(sql_insert);

    } else {
        strcpy(sql_insert, "UPDATE files SET refer=refer+1 WHERE name='");
        strcat(sql_insert, m_upload_file);
        strcat(sql_insert, "' AND ftype='");
        strcat(sql_insert, m_file_type);
        strcat(sql_insert, "' AND fsize=");
        strcat(sql_insert, std::to_string(m_file_size).c_str());
        LOG_INFO(sql_insert);
    }
    m_lock.lock();
    int res = mysql_query(mysql, sql_insert);
    m_lock.unlock();

    if (!res) {
        free(sql_insert);
        LOG_ERROR("INSERT files ERROR, res = %d:%s", res, mysql_error(mysql));
        return INTERNAL_ERROR;
    }
    // m_res在外面设置过了
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::userfile_mysql()
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "INSERT INTO user_file(u_name,f_name,f_type,f_size,f_path,change_time) VALUE('");
    strcat(sql_insert, m_username);
    strcat(sql_insert, "', '");
    strcat(sql_insert, m_upload_file);
    strcat(sql_insert, "', '");
    strcat(sql_insert, m_file_type);
    strcat(sql_insert, "', '");
    strcat(sql_insert, std::to_string(m_file_size).c_str());
    strcat(sql_insert, "', '");
    strcat(sql_insert, m_savepath);
    strcat(sql_insert, "', '");
    time_t t = time(NULL);
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char nowtm[30];
    sprintf(nowtm, "%d-%02d-%02d %02d:%02d:%02d",
        my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
        my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec);
    strcat(sql_insert, nowtm);
    strcat(sql_insert, "')");
    LOG_INFO(sql_insert);

    m_lock.lock();
    int res = mysql_query(mysql, sql_insert);
    m_lock.unlock();

    m_tp->RemoveSock(m_upload_file, m_sockfd);

    if (!res) {
        LOG_ERROR("INSERT user_file ERROR, res = %d:%s", res, mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    //在函数进入前已经设置好了m_res
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::downloadsingle_request()
{
}

http_conn::HTTP_CODE http_conn::getall_request(const char* name)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "SELECT f_type,f_path FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "'");
    LOG_INFO(sql_insert);
    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    MYSQL_RES* result = mysql_store_result(mysql);
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);
    strcpy(m_real_file, name);
    strcat(m_real_file, ".filelst");
    LOG_INFO("create file:%s\n", m_real_file);

    ofstream rowfs(m_real_file, ios::out);
    rowfs << "[";
    while (MYSQL_ROW row = mysql_fetch_row(result)) {

        rowfs << "{\"f_type\":\"" << row[0] << "\",";
        rowfs << "\"f_path\":\"" << row[1] << "\"},";
    }
    rowfs << "]";
    rowfs.close();

    free(sql_insert);
    return FILE_REQUEST;
}

bool http_conn::file_exist()
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "SELECT content FROM files WHERE name='");
    strcat(sql_insert, m_upload_file);
    strcat(sql_insert, "' AND ftype='");
    strcat(sql_insert, m_file_type);
    strcat(sql_insert, "' AND fsize='");
    strcat(sql_insert, std::to_string(m_file_size).c_str());
    strcat(sql_insert, "'");
    LOG_INFO(sql_insert);

    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    free(sql_insert);
    MYSQL_RES* result = mysql_store_result(mysql);
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);
    if (MYSQL_ROW row = mysql_fetch_row(result))
        return true;
    return false;
}
bool http_conn::save_file()
{
    printf("in savefile\n");
    printf("%s\nsize:%d\n", m_string, strlen(m_string));
    //判断文件夹是否存在
    if (access(m_upload_file, F_OK) == -1) {
        mkdir(m_upload_file, S_IRWXU);
    }
    FILE* fd = fopen((std::string(m_upload_file) + "/" + std::to_string(m_slice_id)).c_str(), "a");
    if (!fd) {
        LOG_ERROR("open file ERROR:No.%d,%s", errno, strerror(errno));
        return false;
    }
    int rt = fwrite(m_string, m_content_length, 1, fd);
    fclose(fd);
    LOG_INFO("write %d bytes of file %s", rt, m_upload_file);
    return true;
}
void http_conn::unmap()
{
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1) {
        //以顺序m_iv从各缓冲区中聚集输出数据到m_sockfd
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger) { // Keep-alive
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char* format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() && add_blank_line();
}
bool http_conn::add_headers(int content_len, char* auth)
{
    return add_authorization(auth) && add_content_length(content_len) && add_linger() && add_blank_line();
}
bool http_conn::add_authorization(char* auth)
{
    return add_response("Authorization:%s\r\n", auth);
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret) {
    case INTERNAL_ERROR: {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST: {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST: {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST: {
        add_status_line(200, ok_200_title);
        if (m_res_type == RES_FILE) {
            if (m_with_auth) {
                add_headers(m_file_stat.st_size, m_auth);
                m_with_auth = false;
            } else
                add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf; //写的回复报文
            m_iv[0].iov_len = m_write_idx; //写的总长度
            m_iv[1].iov_base = m_file_address; //文件在内存中的起始地址
            m_iv[1].iov_len = m_file_stat.st_size; //文件大小
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size; //总大小
            return true;
        } else {
            if (m_with_auth) {
                add_headers(m_res.size(), m_auth);
                m_with_auth = false;
            } else
                add_headers(m_res.size());
            if (!add_content(m_res.c_str()))
                return false;
        }
        break;
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    LOG_INFO("client(%s) in process", inet_ntoa(get_address()->sin_addr));
    HTTP_CODE read_ret = process_read();
    printf("read_ret=%d\n", read_ret);
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    printf("write_ret=%d\n", write_ret);
    if (!write_ret) {
        LOG_INFO("in process\n");

        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
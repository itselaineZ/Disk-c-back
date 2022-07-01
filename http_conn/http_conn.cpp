#include "http_conn.h"
#include "../MD5/md5.h"

#include <fstream>
#include <mysql/mysql.h>

//����http��Ӧ��һЩ״̬��Ϣ
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

int GbkToUtf8(char* str_str, size_t src_len, char* dst_str, size_t dst_len)
{
    iconv_t cd;
    char** pin = &str_str;
    char** pout = &dst_str;

    cd = iconv_open("utf8", "gbk");
    if (cd == 0)
        return -1;
    memset(dst_str, 0, dst_len);
    int ori_len = dst_len;
    if (iconv(cd, pin, &src_len, pout, &dst_len) == -1)
        return -1;
    iconv_close(cd);
    *pout = 0;

    return ori_len - dst_len;
}

int Utf8ToGbk(char* src_str, size_t src_len, char* dst_str, size_t dst_len)
{
    iconv_t cd;
    char** pin = &src_str;
    char** pout = &dst_str;

    cd = iconv_open("gbk", "utf8");
    if (cd == 0)
        return -1;
    memset(dst_str, 0, dst_len);
    int dst_ori_len = dst_len;
    if (iconv(cd, pin, &src_len, pout, &dst_len) == -1)
        return -1;
    iconv_close(cd);
    *pout = 0;

    return dst_ori_len - dst_len;
}

void http_conn::initmysql_result(connection_pool* connPool)
{
    //�ȴ����ӳ���ȡһ������
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //��user���м���username��passwd���ݣ������������
    if (mysql_query(mysql, "SELECT name,password FROM users")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //�ӱ��м��������Ľ����
    MYSQL_RES* result = mysql_store_result(mysql);

    //���ؽ�����е�����
    int num_fields = mysql_num_fields(result);

    //���������ֶνṹ������
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    //�ӽ�����л�ȡ��һ�У�����Ӧ���û��������룬����map��
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

//���ļ����������÷�����
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//���ں��¼���ע����¼���ѡ����EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

    // event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    event.events = EPOLLIN | EPOLLET;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//���ں�ʱ���ɾ��������
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//���¼�����ΪEPOLLONESHOT����֤ͬһSOCKETֻ�ܱ�һ���̴߳��������Խ����̡߳�
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT;
    // event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    //  event.events = ev | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//���������ΪMD5��ʽ
string ToKey(string Password)
{
    MD5 md5; //����MD5����
    md5.update(Password);
    string strKey = md5.toString(); //ת��Ϊkey��Կ
    md5.reset(); //����MD5
    string cstrKey(strKey.c_str()); // key��ʽת��ΪCString
    return cstrKey; //���ص���MD5���ܵ�����
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//�ر����ӣ��ر�һ�����ӣ��ͻ�������һ
void http_conn::close_conn(bool real_close)
{
    LOG_INFO("free %d size to %d", READ_BUFFER_SIZE, m_sockfd);
    free(m_read_buf);
    if (real_close && (m_sockfd != -1)) {
        LOG_INFO("clear client(%d)", m_sockfd);
        // clear_auth();
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//����auth
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

//��ʼ������,�ⲿ���ó�ʼ���׽��ֵ�ַ
void http_conn::init(int sockfd, const sockaddr_in& addr, char* root,
    int close_log, string user, string passwd, string sqlname, TASKPOOL* tskpool)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    //�������������������ʱ����������վ��Ŀ¼�����http��Ӧ��ʽ������߷��ʵ��ļ���������ȫΪ��
    doc_root = root;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    m_tp = tskpool;

    LOG_INFO("alloc %d size to %d", READ_BUFFER_SIZE, m_sockfd);
    m_read_buf = (char*)malloc(READ_BUFFER_SIZE * sizeof(char));

    init();
}

//��ʼ���½��ܵ�����
// check_stateĬ��Ϊ����������״̬
void http_conn::init()
{
    m_inpool = false;
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
    m_boundary = 0;

    // memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//��״̬�������ڷ�����һ������
//����ֵΪ�еĶ�ȡ״̬����LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (m_check_state != CHECK_STATE_CONTENT) {
            if (temp == '\r') {
                if ((m_checked_idx + 1) == m_read_idx)
                    return LINE_OPEN;
                else if (m_read_buf[m_checked_idx + 1] == '\n') { //��\r\n����00
                    m_read_buf[m_checked_idx++] = '\0';
                    m_read_buf[m_checked_idx++] = '\0';
                    return LINE_OK;
                }
                return LINE_BAD;
            } else if (temp == '\n') {
                if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') { //��\r\n����00
                    m_read_buf[m_checked_idx - 1] = '\0';
                    m_read_buf[m_checked_idx++] = '\0';
                    return LINE_OK;
                }
                return LINE_BAD;
            }
        } else { // content�ﲻ��Ҫ����
            return LINE_OK;
        }
    }
    return LINE_OPEN;
}

//ѭ����ȡ�ͻ����ݣ�ֱ�������ݿɶ���Է��ر�����
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    int bytes_read = 0;

    while (1) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        LOG_INFO("bytes_read:%d", bytes_read);
        // LOG_INFO("%s\n-----------",m_read_buf+m_read_idx);
        if (bytes_read == 0)
            return false;
        else if (bytes_read < 0 && errno == EAGAIN) {
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            return true;
        }
        // char* p = (char*)malloc(sizeof(char) * (bytes_read + 5));
        // int rt = Utf8ToGbk(m_read_buf + m_read_idx, bytes_read, p, bytes_read);
        // strcpy(m_read_buf + m_read_idx, p);
        // if (rt != -1)
        //     bytes_read = rt;
        // LOG_INFO("%s", m_read_buf+m_read_idx);
        m_read_idx += bytes_read;
        LOG_INFO("m_read_idx:%d, bytes_read:%d", m_read_idx, bytes_read);
        // modfd(m_epollfd, m_sockfd, EPOLLIN);
    }
}

//����http�����У�������󷽷���Ŀ��url��http�汾��
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    m_url = strpbrk(text, " \t"); //�ҵ�һ�γ���spc��tab��λ��
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
    m_url += strspn(m_url, " \t"); //����spc��tab�Ĳ��ֵĳ���
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
    //��urlΪ/ʱ����ʾ�жϽ���
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//����http�����һ��ͷ����Ϣ
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
        *text++ = '\0';
        text = strpbrk(text, "=") + 1;
        m_boundary = text;
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

//�ж�http�����Ƿ���������
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    // printf("m_read_idx:%d,m_content_length:%d,m_checked_idx:%d\n", m_read_idx, m_content_length, m_checked_idx);
    //   printf("text:%s\n", text);
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        m_string = text;
        // LOG_INFO("%s\n-------------", m_string);
        // char* p = (char*)malloc(sizeof(char) * (m_content_length + 5));
        // int rt = Utf8ToGbk(m_string, m_content_length, p, m_content_length);
        // // LOG_INFO("rt=%d",rt);
        // strcpy(m_string, p);
        // free(p);
        // if (rt != -1)
        //     m_content_length = rt;
        // m_string[m_content_length] = '\0';
        // LOG_INFO("%s\n-------------", m_string);
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
            m_start_line = m_checked_idx; //��һ�ζ��Ŀ�ʼ��ַ�ƶ�����ǰ�м���λ��
            LOG_INFO("%s", text);
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER: {
            m_start_line = m_checked_idx; //��һ�ζ��Ŀ�ʼ��ַ�ƶ�����ǰ�м���λ��
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

    //����cgi
    if (cgi == 1) {
        if (*(p + 1) == '0' || *(p + 1) == '1') {
            //���û�����������ȡ����
            // username=123&password=123
            char name[100], password[100];
            int i;
            for (i = 9; m_string[i] != '&'; ++i)
                name[i - 9] = m_string[i];
            name[i - 9] = '\0';
            // int len1 = strlen(name), len2 = 300;
            // char* pa = (char*)malloc(sizeof(char)*300);
            // Utf8ToGbk(name, len1, pa, len2);
            // strcpy(name, pa);
            // free(pa);

            int j = 0;
            for (i = i + 10; m_string[i] != '\0' && m_string[i] != '\t' && m_string[i] != ' '; ++i, ++j)
                password[j] = m_string[i];
            password[j] = '\0';

            if (!name[0] || !password[0]) {
                LOG_ERROR("GET Name/password failed");
                return BAD_REQUEST;
            }
            if (m_with_auth)
                gen_auth();
            if (*(p + 1) == '1') { //ע��
                m_res_type = RES_MESSAGE;
                return register_request(name, password);
            } else { // ��¼
                m_res_type = RES_MESSAGE;
                return login_request(name, password);
            }
        } else if (*(p + 1) == '2' || *(p + 1) == '8' || *(p + 1) == 'a' || *(p + 1) == 'b') {
            // path=xx&username=xx
            char* pth = strstr(m_string, "path=");
            if (!pth)
                return BAD_REQUEST;
            pth += 5;
            char* name = pth;
            name = strpbrk(pth, "&");
            *name++ = '\0';
            name += 9;
            m_string = name;
            char* pa = strpbrk(name, " &\t,\r\n");
            if (pa) {
                *(pa) = '\0';
                m_string = pa + 1;
            }
            // int len1 = strlen(name), len2 = 300;
            // char* pa = (char*)malloc(sizeof(char)*300);
            // Utf8ToGbk(name, len1, pa, len2);
            // strcpy(name, pa);
            // len1 = strlen(pth);

            // free(pa);
            LOG_INFO("query: pth=%s, username=%s\n", pth, name);
            m_username = name;
            if (*(p + 1) == '2') { // ��ѯ�ļ�
                m_res_type = RES_FILE;
                HTTP_CODE rt = getdir_request(name, pth);
                if (rt == INTERNAL_ERROR)
                    return INTERNAL_ERROR;
            } else if (*(p + 1) == '8') { // ɾ���ļ�/�ļ���
                m_res_type = RES_MESSAGE;
                return delfile_request(name, pth);
            } else if (*(p + 1) == 'a') { // 'a' ���ص��ļ�
                // m_res_type���պ�����Ҫ��ʵ��
                m_savepath = pth;
                HTTP_CODE rt = downloadsingle_request(name, pth);
                if (rt == INTERNAL_ERROR)
                    return INTERNAL_ERROR;
                if (m_res_type == RES_MESSAGE)
                    return rt;
            } else { // 'b' ��;ȡ���ϴ�������
                m_res_type = RES_MESSAGE;
                return delbreakpoint_request(name, pth);
            }
        } else if (*(p + 1) == '3') { // ����Ŀ¼
            m_res_type = RES_MESSAGE;
            return mkdir_request();
        } else if (*(p + 1) == '4' || *(p + 1) == 'd') {
            // username=xx
            char* name = strstr(m_string, "username=");
            if (!name)
                return BAD_REQUEST;
            name += 9;
            char* pa = strpbrk(name, " \t\n\r");
            if (pa)
                *pa = '\0';
            printf("name:%s\n", name);
            if (*(p + 1) == '4') { // ��ѯ�ļ�δ���״̬
                m_res_type = RES_FILE;
                HTTP_CODE rt = getstate_request(name);
                if (rt == INTERNAL_ERROR)
                    return INTERNAL_ERROR;
            } else { // ��ĳ���û������ļ��С��ļ�
                m_res_type = RES_FILE;
                HTTP_CODE rt = getall_request(name);
                if (rt == INTERNAL_ERROR)
                    return INTERNAL_ERROR;
            }
        } else if (*(p + 1) == '5' || *(p + 1) == '6' || *(p + 1) == '7') {
            // username=xx&oldpath=xx&newpath=xx
            char* name = strstr(m_string, "username=");
            if (!name)
                return BAD_REQUEST;
            name += 9;
            char* oldpath = name;
            oldpath = strpbrk(oldpath, "&");
            if (!oldpath)
                return BAD_REQUEST;
            *oldpath++ = '\0';
            oldpath += 8;
            char* newpath = oldpath;
            newpath = strpbrk(newpath, "&");
            if (!newpath)
                return BAD_REQUEST;
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
            if (*(p + 1) == '5') { // ���ļ�/���ļ��и���
                return changename_request(name, oldpath, newpath);
            } else if (*(p + 1) == '6') { // �ļ�/�ļ����ƶ�
                return movefile_request(name, oldpath, newpath);
            } else { // �ļ�/�ļ��и���
                return copyfile_request(name, oldpath, newpath);
            }
        } else if (*(p + 1) == '9') { // �����ļ��ϴ�
            // username=xx&filename=xx&filetype=xx&filesize=xx&filepath=xx
            m_res_type = RES_MESSAGE;
            return uploadsingle_request();
        } else if (*(p + 1) == 'c') { // �ļ�������
            // username=xx&dirpath=xx&needslice=hello/dirdone/0...
            char* name = strstr(m_string, "username=");
            if (!name)
                return BAD_REQUEST;
            name += 9;
            char* dirpath = strpbrk(name, "&");
            if (!dirpath)
                return BAD_REQUEST;
            *dirpath++ = '\0';
            dirpath += 8;
            char* pa = strpbrk(dirpath, " \t\n\r");
            if (pa)
                *pa = '\0';
            // m_res_type�ɺ����ھ���
            HTTP_CODE rt = downloaddir_request(name, dirpath);
            if (rt == INTERNAL_ERROR)
                return INTERNAL_ERROR;
            if (m_res_type == RES_MESSAGE)
                return FILE_REQUEST;
        } else {
            return BAD_REQUEST;
        }
    }

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH)) //�������Ȩ��
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode)) //Ŀ¼
        return BAD_REQUEST;
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::register_request(const char* name, const char* password)
{
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    //�����ע�ᣬ�ȼ�����ݿ����Ƿ���������
    //û�������ģ�������������
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
        LOG_INFO("Register Name Repeated, client(%s)\n", inet_ntoa(m_address.sin_addr));
    }
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::login_request(const char* name, const char* password)
{
    //����ǵ�¼��ֱ���ж�
    //���������������û����������ڱ��п��Բ��ҵ�������0�����򷵻�-1
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
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
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
    printf("name:%s\n", name);
    strcpy(m_real_file, name);
    strcat(m_real_file, ".filelst");
    LOG_INFO("create file:%s\n", m_real_file);

    ofstream rowfs(m_real_file, ios::out);
    rowfs << "[";
    char* p = (char*)malloc(sizeof(char) * 500);
    MYSQL_ROW row;
    while (row = mysql_fetch_row(result)) {
        int len = strlen(row[0]);
        int rt = GbkToUtf8(row[0], len, p, 100);
        rowfs << "{\"f_path\":\"" << p << "\",";
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
    free(p);
    rowfs << "]";
    rowfs.close();
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::mkdir_request()
{
    // path=xx&username=xx
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    char* pth = strstr(m_string, "path=");
    if (!pth)
        return BAD_REQUEST;
    pth += 5;
    char* name = pth;
    name = strpbrk(pth, "&");
    if (!name)
        return BAD_REQUEST;
    *name++ = '\0';
    name += 9;
    char* p = strpbrk(name, " \t,\r\n");
    if (p)
        *(p) = '\0';
    LOG_INFO("create: pth=%s, username=%s\n", pth, name);
    strcpy(sql_insert, "SELECT f_path FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path='");
    strcat(sql_insert, pth);
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
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row) {
        m_res = "-2"; // �ظ�Ŀ¼
        free(sql_insert);
        LOG_INFO("duplicate dir:%s", pth);
        return FILE_REQUEST;
    }

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
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    strcpy(sql_insert, "SELECT f_path, f_size, f_type, breakpoint, op, f_name FROM file_breakpoint WHERE u_name='");
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
    char* p = (char*)malloc(sizeof(char) * 500);
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        int len = strlen(row[0]);
        GbkToUtf8(row[0], len, p, 500);
        rowfs << "{\"f_path\":\"" << p << "\",";
        rowfs << "\"f_size\":" << row[1] << ",";
        rowfs << "\"f_type\":\"" << row[2] << "\",";
        rowfs << "\"breakpoint\":" << row[3] << ",";
        rowfs << "\"op\":\"" << row[4] << "\",";
        rowfs << "\"f_name\":\"" << row[5] << "\"},\n";
    }
    free(p);
    rowfs << "]";
    rowfs.close();
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::changename_request(const char* name, const char* oldpath, const char* newpath)
{
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    // strcpy(sql_insert, "UPDATE user_file SET f_path='");
    // strcat(sql_insert, newpath);
    // strcat(sql_insert, "' WHERE u_name='");
    // strcat(sql_insert, name);
    // strcat(sql_insert, "' AND f_path='");
    // strcat(sql_insert, oldpath);
    // strcat(sql_insert, "'");
    // LOG_INFO(sql_insert);

    // m_lock.lock();
    // int res = mysql_query(mysql, sql_insert);
    // m_lock.unlock();

    strcpy(sql_insert, "SELECT f_path FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND (f_path REGEXP '");
    strcat(sql_insert, oldpath);
    strcat(sql_insert, "/.*' OR f_path='");
    strcat(sql_insert, oldpath);
    strcat(sql_insert, "')");
    LOG_INFO(sql_insert);
    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    MYSQL_RES* result = mysql_store_result(mysql);
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD* fields = mysql_fetch_fields(result);
    int len = strlen(oldpath), res = 0;
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        strcpy(sql_insert, "UPDATE user_file SET f_path='");
        if (strlen(row[0]) != len)
            strcat(sql_insert, (std::string(newpath) + std::string(row[0]).substr(len)).c_str());
        else
            strcat(sql_insert, newpath);
        strcat(sql_insert, "' WHERE u_name='");
        strcat(sql_insert, name);
        strcat(sql_insert, "' AND f_path='");
        strcat(sql_insert, row[0]);
        strcat(sql_insert, "'");
        LOG_INFO(sql_insert);
        m_lock.lock();
        res = mysql_query(mysql, sql_insert);
        m_lock.unlock();
        if (res) {
            m_res = "-1";
            free(sql_insert);
            LOG_ERROR("UPDATE user_file f_path ERROR, res = %d:%s\n", res, mysql_error(mysql));
            return INTERNAL_ERROR;
        }
    }

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
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
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
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
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
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    strcpy(sql_insert, "SELECT f_name,f_type,f_size FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND (f_path='");
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
    strcat(sql_insert, "' AND (f_path='");
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
            LOG_INFO(sql_insert);

            m_lock.lock();
            res = mysql_query(mysql, sql_insert);
            m_lock.unlock();
            system(("rm -rf " + std::string(row[0])).c_str());
            if (res) {
                m_res = "-1";
                LOG_ERROR("UPDATE files refer ERROR, res = %d:%s\n", res, mysql_error(mysql));
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
    char* p = strstr(m_string, "username=");
    if (!p)
        return BAD_REQUEST;
    p += 9;
    m_username = p;
    p = strpbrk(p, "&");
    if (!p)
        return BAD_REQUEST;
    *p++ = '\0';
    p += 9;
    m_savepath = p;
    p = strpbrk(p, "\n\r");
    *p = '\0';
    m_string = p + 1;
    if (file_exist()) { // �Ƿ����ݿ����Ѿ����ļ�
        LOG_INFO("file exists.");
        m_res = "{\"already\":1, \"all\":1}";
        HTTP_CODE rt = insertfile_mysql(UPDATE_MYSQL);
        if (rt == INTERNAL_ERROR)
            return INTERNAL_ERROR;
        return userfile_mysql();
    } else {
        if (m_string[0] == 'u') { // �״��ύ�ϴ�����
            // upload
            LOG_INFO("upload REQUEST: username=%s,filemd5=%s,filetype=%s,filesize=%d,filepath=%s",
                m_username, m_upload_file, m_file_type, m_file_size, m_savepath);
            LOG_INFO("add task:%d", m_sockfd);
            m_tp->AddTask(m_upload_file, m_file_size, m_sockfd, m_file_type);
        } else if (m_string[0] == 'w') { // �޷������ݴ�������µı�����ϵ��ͬ������
            LOG_INFO("in waiting");
        } else { // multipart/form-data��������
            LOG_INFO("to save:%d", m_sockfd);
            // ��ǰ��������Ƭ��û�б��洢��
            if (!m_tp->FindSlice(m_upload_file, m_slice_id)) {
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
                    int already = m_tp->GetRecvSize(m_upload_file);
                    int all = m_tp->GetAllSize(m_upload_file);
                    m_res = "{\"already\":" + std::to_string(already)
                        + ", \"all\":" + std::to_string(all)
                        + "}";
                    HTTP_CODE rt = breakpoint_mysql(DELETE_MYSQL, "up", 0);
                    rt = insertfile_mysql(INSERT_MYSQL, all);
                    if (rt == INTERNAL_ERROR)
                        return INTERNAL_ERROR;
                    return userfile_mysql();
                }
            }
        }
        int offset_begin = m_tp->GetSlice(m_upload_file);
        int already = m_tp->GetRecvSize(m_upload_file);
        int all = m_tp->GetAllSize(m_upload_file);
        int slicesize = m_tp->GetSliceSize();
        if (!all) {
            HTTP_CODE rt = insertfile_mysql(INSERT_MYSQL, all);
            if (rt == INTERNAL_ERROR)
                return INTERNAL_ERROR;
            return userfile_mysql();
        }

        HTTP_CODE rt;
        if (m_string[0] == 'u')
            rt = breakpoint_mysql(INSERT_MYSQL, "up", already * slicesize);
        else
            rt = breakpoint_mysql(UPDATE_MYSQL, "up", already * slicesize);
        if (rt == INTERNAL_ERROR)
            return INTERNAL_ERROR;

        if (offset_begin == -1) { // ����ʧ
            LOG_ERROR("task lost:%s", m_upload_file);
            return INTERNAL_ERROR;
        } else if (offset_begin < -1) { // �ȴ�����client�ϴ���ɣ�û��ʣ��slice�������ڷ���
            LOG_INFO("client(%d:%s) waiting: all slice assigned", m_sockfd, m_username);
            m_res = "{\"already\":" + std::to_string(already)
                + ", \"all\":" + std::to_string(all)
                + "}";
            return FILE_REQUEST;
        } else { // �ɹ����䵽slice��Ҫ���ϴ�
            m_slice_assign = offset_begin / slicesize;
            m_res = "{\"slice\":" + std::to_string(m_slice_assign)
                + ", \"need_offset_begin\":" + std::to_string(offset_begin)
                + ", \"slice_size\":" + std::to_string(slicesize)
                + ", \"already\":" + std::to_string(already)
                + ", \"all\":" + std::to_string(all)
                + "}";
            return FILE_REQUEST;
        }
    }
}

http_conn::HTTP_CODE http_conn::insertfile_mysql(MYSQL_OP flg, const int all)
{
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    if (flg == INSERT_MYSQL) {
        strcpy(sql_insert, "INSERT INTO files(name,ftype,fsize,content,refer,slice) VALUES('");
        strcat(sql_insert, m_upload_file);
        strcat(sql_insert, "', '");
        strcat(sql_insert, m_file_type);
        strcat(sql_insert, "', ");
        strcat(sql_insert, std::to_string(m_file_size).c_str());
        strcat(sql_insert, ", '");
        strcat(sql_insert, m_upload_file);
        strcat(sql_insert, "', 1, ");
        strcat(sql_insert, std::to_string(all).c_str());
        strcat(sql_insert, ")");
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

    if (res) {
        free(sql_insert);
        LOG_ERROR("INSERT files ERROR, res = %d:%s", res, mysql_error(mysql));
        return INTERNAL_ERROR;
    }
    // m_res���������ù���
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::userfile_mysql()
{
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    strcpy(sql_insert, "INSERT INTO user_file(u_name,f_name,f_type,f_size,f_path,change_time) VALUE('");
    strcat(sql_insert, m_username);
    strcat(sql_insert, "', '");
    strcat(sql_insert, m_upload_file);
    strcat(sql_insert, "', '");
    strcat(sql_insert, m_file_type);
    strcat(sql_insert, "', ");
    strcat(sql_insert, std::to_string(m_file_size).c_str());
    strcat(sql_insert, ", '");
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

    if (res) {
        LOG_ERROR("INSERT user_file ERROR, res = %d:%s", res, mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    //�ں�������ǰ�Ѿ����ú���m_res
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::downloadsingle_request(const char* name, const char* pth)
{
    int slice = 0;
    char* p = strstr(m_string, "needslice=");
    if (p) {
        p += 10;
        char* q = strpbrk(p, " \t\n\r");
        if (q)
            *q = '\0';
        slice = atoi(p);
        LOG_INFO("%d", slice);
    }
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    strcpy(sql_insert, "SELECT f_name,f_type,f_size FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path='");
    strcat(sql_insert, pth);
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
    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) {
        LOG_ERROR("didn't find row u_name='%s' and f_path='%s'", name, pth);
        free(sql_insert);
        return NO_RESOURCE;
    }
    char uploadfile[40], file_type[10];
    m_file_size = atoi(row[2]);
    strcpy(uploadfile, row[0]);
    strcpy(file_type, row[1]);
    m_upload_file = uploadfile;
    m_file_type = file_type;
    LOG_INFO("m_upload_file=%s,m_file_type=%s,m_file_size=%d", m_upload_file, m_file_type, m_file_size);
    strcpy(sql_insert, "SELECT content,slice FROM files WHERE name='");
    strcat(sql_insert, row[0]);
    strcat(sql_insert, "' AND ftype='");
    strcat(sql_insert, row[1]);
    strcat(sql_insert, "' AND fsize=");
    strcat(sql_insert, row[2]);
    LOG_INFO(sql_insert);
    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    result = mysql_store_result(mysql);
    num_fields = mysql_num_fields(result);
    fields = mysql_fetch_fields(result);
    row = mysql_fetch_row(result);
    if (!row) {
        LOG_ERROR("didn't find row f_name='%s' and f_type='%s' and f_size='%s'", row[0], row[1], row[2]);
        free(sql_insert);
        return NO_RESOURCE;
    }

    if (slice == 1) {
        breakpoint_mysql(INSERT_MYSQL, "down", slice);
    } else if (slice > 1) {
        breakpoint_mysql(UPDATE_MYSQL, "down", slice);
    }

    int all = 0;
    if (row[1])
        all = atoi(row[1]);
    LOG_INFO("all=%d,slice=%d", all, slice);
    if (slice >= all) {
        m_res = "OK";
        m_res_type = RES_MESSAGE;
    } else {
        strcpy(m_real_file, row[0]);
        strcat(m_real_file, "/");
        strcat(m_real_file, std::to_string(slice).c_str());
        LOG_INFO("m_real_file:%s", m_real_file);
        m_res_type = RES_FILE;
    }
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::getall_request(const char* name)
{
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
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
    char* p = (char*)malloc(sizeof(char) * 500);
    while (MYSQL_ROW row = mysql_fetch_row(result)) {

        rowfs << "{\"f_type\":\"" << row[0] << "\",";
        int len = strlen(row[1]);
        GbkToUtf8(row[1], len, p, 500);
        rowfs << "\"f_path\":\"" << p << "\"},";
    }
    free(p);
    rowfs << "]";
    rowfs.close();

    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::breakpoint_mysql(MYSQL_OP flg, const char* op, const int already)
{
    LOG_INFO("before this malloc brk");
    char* brk_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    LOG_INFO("after this free brk");
    if (flg == INSERT_MYSQL) {
        strcpy(brk_insert, "INSERT INTO file_breakpoint(u_name,f_name,f_size,f_type,op,breakpoint,f_path) VALUES('");
        strcat(brk_insert, m_username);
        strcat(brk_insert, "', '");
        strcat(brk_insert, m_upload_file);
        strcat(brk_insert, "', ");
        strcat(brk_insert, std::to_string(m_file_size).c_str());
        strcat(brk_insert, ", '");
        strcat(brk_insert, m_file_type);
        strcat(brk_insert, "', '");
        strcat(brk_insert, op);
        strcat(brk_insert, "', ");
        strcat(brk_insert, std::to_string(already).c_str());
        strcat(brk_insert, ", '");
        strcat(brk_insert, m_savepath);
        strcat(brk_insert, "') ON DUPLICATE KEY UPDATE breakpoint=");
        strcat(brk_insert, std::to_string(already).c_str());
    } else if (flg == UPDATE_MYSQL) {
        strcpy(brk_insert, "UPDATE file_breakpoint SET breakpoint=");
        strcat(brk_insert, std::to_string(already).c_str());
        strcat(brk_insert, " WHERE u_name='");
        strcat(brk_insert, m_username);
        strcat(brk_insert, "' AND f_path='");
        strcat(brk_insert, m_savepath);
        strcat(brk_insert, "'");
    } else { // DELETE
        strcpy(brk_insert, "DELETE FROM file_breakpoint WHERE u_name='");
        strcat(brk_insert, m_username);
        strcat(brk_insert, "' AND f_path='");
        strcat(brk_insert, m_savepath);
        strcat(brk_insert, "'");
    }
    LOG_INFO(brk_insert);

    m_lock.lock();
    int res = mysql_query(mysql, brk_insert);
    m_lock.unlock();

    if (res) {
        LOG_ERROR("modify file_breakpoint error, res = %d:%s", res, mysql_error(mysql));
        free(brk_insert);
        return INTERNAL_ERROR;
    }
    // m_res�������Ѿ���д����
    free(brk_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::downloaddir_request(const char* name, const char* dirpath)
{
    m_res_type = RES_FILE;
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
    strcpy(sql_insert, "SELECT f_path FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path REGEXP '");
    strcat(sql_insert, dirpath);
    strcat(sql_insert, "/.*' AND f_type='dir'");
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
    char* p = (char*)malloc(sizeof(char) * 500);
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        int len = strlen(row[0]);
        GbkToUtf8(row[0], len, p, 500);
        rowfs << "{\"dir\":\"" << p << "\"},\n";
    }
    rowfs << "],\n";

    strcpy(sql_insert, "SELECT f_path FROM user_file WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path REGEXP '");
    strcat(sql_insert, dirpath);
    strcat(sql_insert, "/.*' AND f_type<>'dir'");
    LOG_INFO(sql_insert);
    if (mysql_query(mysql, sql_insert)) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    }
    result = mysql_store_result(mysql);
    num_fields = mysql_num_fields(result);
    fields = mysql_fetch_fields(result);
    rowfs << "[";
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        int len = strlen(row[0]);
        GbkToUtf8(row[0], len, p, 500);
        rowfs << "{\"file\":\"" << p << "\"},\n";
    }
    free(p);
    rowfs << "]";
    rowfs.close();
    free(sql_insert);
    return FILE_REQUEST;
}

http_conn::HTTP_CODE http_conn::delbreakpoint_request(const char* name, const char* pth)
{
    char* sql_insert = (char*)malloc(sizeof(char) * 200);
    strcpy(sql_insert, "SELECT f_name FROM file_breakpoint WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path='");
    strcat(sql_insert, pth);
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
    MYSQL_ROW row = mysql_fetch_row(result);
    system(("rm -rf " + std::string(row[0])).c_str());

    strcpy(sql_insert, "DELETE FROM file_breakpoint WHERE u_name='");
    strcat(sql_insert, name);
    strcat(sql_insert, "' AND f_path='");
    strcat(sql_insert, pth);
    strcat(sql_insert, "'");
    LOG_INFO(sql_insert);

    m_lock.lock();
    int res = mysql_query(mysql, sql_insert);
    m_lock.unlock();

    if (res) {
        LOG_ERROR("DELETE file_breakpoint ERROR, res = %d:%s", res, mysql_errno(mysql));
        free(sql_insert);
        return INTERNAL_ERROR;
    } else
        m_res = "0";
    free(sql_insert);
    return FILE_REQUEST;
}

bool http_conn::file_exist()
{
    char* sql_insert = (char*)malloc(sizeof(char) * SQL_CLAUS_LEN);
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
    LOG_INFO("in savefile()");
    ++m_string;
    // LOG_INFO("m_string:(%d)%p,m_boundary:%s", strlen(m_string), m_string, m_boundary);
    LOG_INFO("%s", m_string);
    char* content_end = strstr(m_string, m_boundary);
    for (content_end - 1; content_end && *content_end == '-'; --content_end)
        ;
    content_end--;
    LOG_INFO("ced:%p", content_end);
    // LOG_INFO("alive here");
    //  ���յ�Ƭ�Σ�����������д��
    m_tp->RecvTask(m_upload_file, m_slice_id);
    //�ж��ļ����Ƿ����
    if (access(m_upload_file, F_OK) == -1) {
        mkdir(m_upload_file, S_IRWXU);
    }
    int ori_len = m_content_length;
    // int new_len = ori_len + ori_len / 2 + 1;
    // char* p = (char*)malloc(sizeof(char) * new_len);

    // LOG_INFO("alive there");
    // int sz = GbkToUtf8(m_string, ori_len, p, new_len);
    // LOG_INFO("changed back to utf8");
    FILE* fd = fopen((std::string(m_upload_file) + "/" + std::to_string(m_slice_id)).c_str(), "w");
    if (!fd) {
        LOG_ERROR("open file ERROR:No.%d,%s", errno, strerror(errno));
        return false;
    }
    LOG_INFO("content_length:%d,boundary_len:%d", m_content_length, strlen(m_boundary));
    LOG_INFO("%s", m_boundary);
    int sz = m_content_length - strlen(m_boundary) - 4;
    if (content_end > m_string)
        sz = int(content_end - m_string);
    // int rt = fwrite(m_string, m_content_length, 1, fd);
    int rt = fwrite(m_string, sz, 1, fd);
    // int rt = fwrite(p, sz, 1, fd);
    fclose(fd);
    // free(p);
    LOG_INFO("write %d bytes, slice %d of file %s", m_content_length, m_slice_id, m_upload_file);
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
        //��˳��m_iv�Ӹ��������оۼ�������ݵ�m_sockfd
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
            m_iv[0].iov_base = m_write_buf; //д�Ļظ�����
            m_iv[0].iov_len = m_write_idx; //д���ܳ���
            m_iv[1].iov_base = m_file_address; //�ļ����ڴ��е���ʼ��ַ
            m_iv[1].iov_len = m_file_stat.st_size; //�ļ���С
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size; //�ܴ�С
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
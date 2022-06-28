#ifndef H_TASK
#define H_TASK

#include "../file/file.h"
#include "../lock/locker.h"
#include <queue>
#include <set>
#include <string>
using namespace std;

enum TASK_STATUS {
    MERGE_FAILED = 0,
    MERGE_SUCCESS,
    RECVING,
    REC_ERROR
};

class taskque {
public:
    static const int SLICE_SIZE = 512; // 1KBÿƬ
    taskque();
    taskque(int t_size, string type, string name, bool init);
    ~taskque();
    int GetSlice();
    TASK_STATUS RecvTask(int slice);
    int GetRecvSize();
    int GetAllSize();
    static int GetSliceSize();
    void AddSlice(int slice);
    bool FindSlice(int slice);

public:
    string m_type;
    int m_size;

private:
    locker lck;
    string m_name;
    int m_count;
    queue<int> m_q;
};

class cliset {
public:
    cliset();
    ~cliset();
    void AddSock(int sock);
    bool FindSock(int sock);
    bool DelSock(int sock);

private:
    locker lck;
    set<int> m_st;
};

#endif
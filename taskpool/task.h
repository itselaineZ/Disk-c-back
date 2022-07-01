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
    static const int SLICE_SIZE = 1024*1024; // 2MBÿƬ
    taskque();
    taskque(int t_size, string type, string name);
    ~taskque();
    int GetSlice();
    TASK_STATUS RecvTask(int slice);
    int GetRecvSize();
    int GetAllSize();
    static int GetSliceSize();
    bool FindSlice(int slice);

public:
    string m_type;
    int m_size;

private:
    locker lck;
    string m_name;
    int m_count;
    queue<int> m_assign;
    set<int> m_rec;
};

#endif
#ifndef H_TASKPOOL
#define H_TASKPOOL

#include "../lock/locker.h"
#include "../log/log.h"
#include "task.h"
#include <map>
#include <set>

class TASKPOOL {
public:
    static TASKPOOL* GetInstance();
    bool AddTask(string task, int t_size, int sock, string type);
    int GetSlice(string task);
    void DelTask(string task);
    TASK_STATUS RecvTask(string task, int slice);
    int GetSliceSize();
    int GetRecvSize(string task);
    int GetAllSize(string task);
    string GetTaskType(string task);
    int GetTaskSize(string task);
    bool FindSlice(string task, int slice);

private:
    TASKPOOL();
    ~TASKPOOL();

    locker lck;
    map<string, taskque*>* m_q;
    //map<string, cliset*>* m_sockpool;
};

#endif
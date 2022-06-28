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
    bool FindSock(int sock, string task);
    string GetTaskType(string task);
    int GetTaskSize(string task);
    void RemoveSock(string task, int sock);
    bool RecoverSlice(string task, int slice);

private:
    TASKPOOL();
    ~TASKPOOL();

    locker lck_asgn, lck_rcv, lck_sock;
    map<string, taskque*>* m_assign;
    map<string, taskque*>* m_rec;
    map<string, cliset*>* m_sockpool;
};

#endif
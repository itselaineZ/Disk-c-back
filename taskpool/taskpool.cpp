#include "taskpool.h"

TASKPOOL::TASKPOOL()
{
    m_q = new map<string, taskque*>;
}

TASKPOOL::~TASKPOOL()
{
    delete m_q;
}

TASKPOOL* TASKPOOL::GetInstance()
{
    static TASKPOOL tp;
    return &tp;
}

// 不存在任务则新建任务
bool TASKPOOL::AddTask(string task, int t_size, int sock, string type)
{
    //不存在该任务，则创建上传该文件的任务
    if (m_q->find(task) == m_q->end()) {

        lck.lock();
        (*m_q)[task] = new taskque(t_size, type, task);
        lck.unlock();

    }
    return false;
}

//获得一个该文件还未分配的片段信息，返回-1表示失败，小于-1代表已经分配完
int TASKPOOL::GetSlice(string task)
{
    if (m_q->find(task) == m_q->end())
        return -1;

    return (*m_q)[task]->GetSlice();
}

void TASKPOOL::DelTask(string task)
{
    if ((*m_q)[task]->GetRecvSize() == (*m_q)[task]->GetAllSize()) {
        delete (*m_q)[task];
        m_q->erase(task);
    }
}

TASK_STATUS TASKPOOL::RecvTask(string task, int slice)
{
    if (m_q->find(task) == m_q->end())
        return REC_ERROR;
    TASK_STATUS rt = (*m_q)[task]->RecvTask(slice);
    return rt;
}

int TASKPOOL::GetSliceSize()
{
    return taskque::GetSliceSize();
}

int TASKPOOL::GetRecvSize(string task)
{
    if (m_q->find(task) == m_q->end())
        return -1;
    return (*m_q)[task]->GetRecvSize();
}

int TASKPOOL::GetAllSize(string task)
{
    if (m_q->find(task) == m_q->end())
        return -1;
    return (*m_q)[task]->GetAllSize();
}

string TASKPOOL::GetTaskType(string task)
{
    if (m_q->find(task) == m_q->end())
        return "";
    return (*m_q)[task]->m_type;
}

int TASKPOOL::GetTaskSize(string task)
{
    if (m_q->find(task) == m_q->end())
        return -1;
    return (*m_q)[task]->m_size;
}

bool TASKPOOL::FindSlice(string task, int slice){
    if (m_q->find(task) == m_q->end())
        return false;
    return (*m_q)[task]->FindSlice(slice);
}